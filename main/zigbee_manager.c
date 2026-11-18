#include "zigbee_manager.h"

#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "diagnostics.h"
#include "esp_check.h"
#include "esp_coexist.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "stage_config.h"
#include "storage_manager.h"
#include "zigbee_state_cache.h"
#include "zigbee_control_retry.h"
#include "zigbee_recovery.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "ezbee/nwk.h"
#include "ezbee/secur.h"

static const char *TAG = "zigbee";
static SemaphoreHandle_t s_devices_lock;
static zb_device_snapshot_t s_devices[APP_MAX_ZIGBEE_DEVICES];
static zb_device_snapshot_t s_persisted_devices[APP_MAX_ZIGBEE_DEVICES];
static bool s_device_storage_healthy = true;
static bool s_network_ready;
static int64_t s_pairing_until_us;
static zb_state_cache_t s_state_cache;
static int64_t s_last_seen_ms[APP_MAX_ZIGBEE_DEVICES];
static int64_t s_last_state_seen_ms[APP_MAX_ZIGBEE_DEVICES];
static zigbee_recovery_state_t s_recovery[APP_MAX_ZIGBEE_DEVICES];
static uint32_t s_control_generation[APP_MAX_ZIGBEE_DEVICES][3];
static QueueHandle_t s_zigbee_io_queue;

#define ZIGBEE_IO_QUEUE_DEPTH 48
#define ZIGBEE_IO_PENDING_CAPACITY 64

typedef enum {
    ZIGBEE_IO_READ,
    ZIGBEE_IO_CONTROL,
    ZIGBEE_IO_CONFIG_REPORT,
} zigbee_io_type_t;

typedef struct {
    zigbee_io_type_t type;
    uint16_t short_addr;
    uint8_t endpoint;
    uint16_t cluster;
    uint16_t attribute;
    bool requested_on;
    uint8_t attempt;
    uint32_t control_generation;
    int64_t origin_ms;
    int64_t due_ms;
} zigbee_io_operation_t;
static zigbee_io_operation_t s_pending_operations[ZIGBEE_IO_PENDING_CAPACITY];
#if LIGHTGW_HAS_DISCOVERY
static uint16_t s_discovery_in_progress[APP_MAX_ZIGBEE_DEVICES];
#endif

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

typedef struct {
    uint16_t short_addr;
    esp_zb_ieee_addr_t ieee;
    uint8_t ep_count;
    uint8_t next_ep;
    uint8_t eps[APP_MAX_ZIGBEE_ENDPOINTS];
    uint8_t onoff_eps[APP_MAX_ZIGBEE_ENDPOINTS];
    uint8_t onoff_count;
} discovery_ctx_t;

#if LIGHTGW_HAS_DISCOVERY
static void discover_device(uint16_t short_addr, const esp_zb_ieee_addr_t ieee);
#endif

static void save_devices(void)
{
    if (!s_device_storage_healthy) {
        storage_manager_record_write("nvs", "devices_v2_write_blocked", ESP_ERR_INVALID_STATE, true);
        return;
    }
    if (memcmp(s_devices, s_persisted_devices, sizeof(s_devices)) == 0) {
        storage_manager_record_write("nvs", "devices_v2", ESP_OK, false);
        return;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("zb_app", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, "devices_v2", s_devices, sizeof(s_devices));
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    storage_manager_record_write("nvs", "devices_v2", err, true);
    if (err == ESP_OK) memcpy(s_persisted_devices, s_devices, sizeof(s_devices));
}

static void load_devices(void)
{
    s_device_storage_healthy = true;
    memset(s_last_seen_ms, 0, sizeof(s_last_seen_ms));
    memset(s_last_state_seen_ms, 0, sizeof(s_last_state_seen_ms));
    memset(s_recovery, 0, sizeof(s_recovery));
    memset(s_control_generation, 0, sizeof(s_control_generation));
    for (size_t i = 0; i < APP_MAX_ZIGBEE_DEVICES; ++i) {
        s_devices[i].state[0] = s_devices[i].state[1] = s_devices[i].state[2] = ZB_CHANNEL_UNKNOWN;
    }
    nvs_handle_t nvs;
    size_t len = sizeof(s_devices);
    esp_err_t open_err = nvs_open("zb_app", NVS_READONLY, &nvs);
    if (open_err == ESP_OK) {
        esp_err_t read_err = nvs_get_blob(nvs, "devices_v2", s_devices, &len);
        if (read_err != ESP_OK && read_err != ESP_ERR_NVS_NOT_FOUND) {
            memset(s_devices, 0, sizeof(s_devices));
            s_device_storage_healthy = false;
            storage_manager_record_corruption("nvs", "devices_v2 unreadable; automatic overwrite blocked");
        } else if (read_err == ESP_OK && len != sizeof(s_devices)) {
            memset(s_devices, 0, sizeof(s_devices));
            s_device_storage_healthy = false;
            storage_manager_record_corruption("nvs", "devices_v2 size mismatch; automatic overwrite blocked");
        }
        nvs_close(nvs);
    } else if (open_err != ESP_ERR_NVS_NOT_FOUND) {
        s_device_storage_healthy = false;
        storage_manager_record_write("nvs", "devices_v2_open", open_err, true);
    }
    for (size_t i = 0; i < APP_MAX_ZIGBEE_DEVICES; ++i) {
        if (s_devices[i].used) {
            s_devices[i].online = false;
            s_devices[i].state[0] = s_devices[i].state[1] = s_devices[i].state[2] = ZB_CHANNEL_UNKNOWN;
        }
    }
    memcpy(s_persisted_devices, s_devices, sizeof(s_devices));
}

static int find_device_by_short(uint16_t short_addr)
{
    for (int i = 0; i < APP_MAX_ZIGBEE_DEVICES; ++i) if (s_devices[i].used && s_devices[i].short_addr == short_addr) return i;
    return -1;
}

#if LIGHTGW_HAS_DISCOVERY
static int find_device_by_ieee(const esp_zb_ieee_addr_t ieee)
{
    for (int i = 0; i < APP_MAX_ZIGBEE_DEVICES; ++i) {
        if (s_devices[i].used && memcmp(s_devices[i].ieee_addr, ieee, sizeof(esp_zb_ieee_addr_t)) == 0) return i;
    }
    return -1;
}

static int find_or_add_device(uint16_t short_addr, const esp_zb_ieee_addr_t ieee)
{
    int free_slot = -1;
    for (int i = 0; i < APP_MAX_ZIGBEE_DEVICES; ++i) {
        if (!s_devices[i].used && free_slot < 0) free_slot = i;
        if (s_devices[i].used && s_devices[i].short_addr == short_addr &&
            memcmp(s_devices[i].ieee_addr, ieee, sizeof(esp_zb_ieee_addr_t)) != 0) {
            s_devices[i].online = false;
            s_devices[i].short_addr = UINT16_MAX;
            s_devices[i].state[0] = s_devices[i].state[1] = s_devices[i].state[2] = ZB_CHANNEL_UNKNOWN;
        }
        if (s_devices[i].used && memcmp(s_devices[i].ieee_addr, ieee, sizeof(esp_zb_ieee_addr_t)) == 0) {
            s_devices[i].short_addr = short_addr;
            s_devices[i].online = true;
            return i;
        }
    }
    if (free_slot >= 0) {
        zb_device_snapshot_t *d = &s_devices[free_slot];
        memset(d, 0, sizeof(*d));
        d->used = true;
        d->online = true;
        d->short_addr = short_addr;
        memcpy(d->ieee_addr, ieee, sizeof(d->ieee_addr));
        d->state[0] = d->state[1] = d->state[2] = ZB_CHANNEL_UNKNOWN;
    }
    return free_slot;
}

/* Resolve the current 16-bit address from the stack's IEEE-address table.
 * A child may receive a new short address after a rejoin while the Web page
 * still carries the previously persisted address.  The IEEE address is the
 * stable identity; never treat this lookup alone as proof of a fresh reply. */
static uint16_t resolve_short_address(const esp_zb_ieee_addr_t ieee,
                                      uint16_t stored_addr,
                                      const char *reason)
{
    if (!s_network_ready) return stored_addr;
    ezb_shortaddr_t resolved = UINT16_MAX;
    esp_zb_lock_acquire(portMAX_DELAY);
    ezb_err_t err = ezb_address_short_by_extended((const ezb_extaddr_t *)ieee, &resolved);
    esp_zb_lock_release();
    if (err != EZB_ERR_NONE || resolved == 0 || resolved == UINT16_MAX) return stored_addr;
    if (resolved == stored_addr) return stored_addr;

    bool changed = false;
    xSemaphoreTake(s_devices_lock, portMAX_DELAY);
    int idx = find_device_by_ieee(ieee);
    if (idx >= 0 && s_devices[idx].short_addr != resolved) {
        uint16_t previous = s_devices[idx].short_addr;
        s_devices[idx].short_addr = resolved;
        s_devices[idx].online = false;
        s_last_seen_ms[idx] = 0;
        s_last_state_seen_ms[idx] = 0;
        s_devices[idx].state[0] = s_devices[idx].state[1] = s_devices[idx].state[2] = ZB_CHANNEL_UNKNOWN;
        zb_state_cache_device_announced(&s_state_cache, ieee, resolved, now_ms());
        zigbee_recovery_begin(&s_recovery[idx], now_ms());
        save_devices();
        changed = true;
        ESP_LOGW(TAG, "Resolved Zigbee short address 0x%04x -> 0x%04x (%s)",
                 previous, resolved, reason ? reason : "address-table");
    }
    xSemaphoreGive(s_devices_lock);
    return changed ? resolved : stored_addr;
}
#else
static uint16_t resolve_short_address(const esp_zb_ieee_addr_t ieee,
                                      uint16_t stored_addr,
                                      const char *reason)
{
    (void)ieee;
    (void)reason;
    return stored_addr;
}
#endif

static uint8_t send_read(uint16_t addr, uint8_t endpoint, uint16_t cluster, uint16_t attr)
{
    esp_zb_zcl_read_attr_cmd_t req = {0};
    req.zcl_basic_cmd.dst_addr_u.addr_short = addr;
    req.zcl_basic_cmd.dst_endpoint = endpoint;
    req.zcl_basic_cmd.src_endpoint = APP_GATEWAY_ENDPOINT;
    req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    req.clusterID = cluster;
    req.attr_number = 1;
    req.attr_field = &attr;
    uint8_t tsn = esp_zb_zcl_read_attr_cmd_req(&req);
    ESP_LOGI(TAG, "ZCL read TX tsn=%u short=0x%04x endpoint=%u cluster=0x%04x attr=0x%04x",
             tsn, addr, endpoint, cluster, attr);
    return tsn;
}

static bool queue_operation(const zigbee_io_operation_t *operation)
{
    if (!s_zigbee_io_queue || !operation || xQueueSend(s_zigbee_io_queue, operation, 0) != pdTRUE) {
        diagnostics_set_error(TAG, "Zigbee I/O queue full");
        return false;
    }
    return true;
}

static bool queue_read(uint16_t addr, uint8_t endpoint, uint16_t cluster, uint16_t attr,
                       int64_t due_ms)
{
    zigbee_io_operation_t operation = {
        .type = ZIGBEE_IO_READ,
        .short_addr = addr,
        .endpoint = endpoint,
        .cluster = cluster,
        .attribute = attr,
        .origin_ms = now_ms(),
        .due_ms = due_ms,
    };
    return queue_operation(&operation);
}

static void queue_all_states(uint16_t addr, const uint8_t eps[3], int64_t first_due_ms)
{
    unsigned queued = 0;
    for (int i = 0; i < 3; ++i) if (eps[i]) {
        queue_read(addr, eps[i], ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                   ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                   first_due_ms + (int64_t)queued * APP_ZIGBEE_READ_STAGGER_MS);
        queued++;
    }
}

static void poll_persisted_devices(void)
{
    struct { uint16_t addr; uint8_t eps[3]; } pending[APP_MAX_ZIGBEE_DEVICES];
    struct { uint16_t addr; esp_zb_ieee_addr_t ieee; } rediscover[APP_MAX_ZIGBEE_DEVICES];
    size_t count = 0;
    size_t rediscover_count = 0;
    xSemaphoreTake(s_devices_lock, portMAX_DELAY);
    for (size_t i = 0; i < APP_MAX_ZIGBEE_DEVICES; ++i) {
        if (s_devices[i].used && s_devices[i].supported && s_devices[i].mapping_confirmed) {
            pending[count].addr = s_devices[i].short_addr;
            memcpy(pending[count].eps, s_devices[i].endpoints, 3);
            for (int ch = 0; ch < 3; ++ch) {
                zb_state_cache_register_endpoint(&s_state_cache, s_devices[i].ieee_addr,
                                                 s_devices[i].short_addr, s_devices[i].endpoints[ch]);
            }
            count++;
            zigbee_recovery_begin(&s_recovery[i], now_ms());
        } else if (s_devices[i].used && s_devices[i].short_addr != 0) {
            rediscover[rediscover_count].addr = s_devices[i].short_addr;
            memcpy(rediscover[rediscover_count].ieee, s_devices[i].ieee_addr,
                   sizeof(esp_zb_ieee_addr_t));
            rediscover_count++;
        }
    }
    xSemaphoreGive(s_devices_lock);
    int64_t due_ms = now_ms() + APP_ZIGBEE_RECOVERY_DELAY_MS;
    for (size_t i = 0; i < count; ++i) {
        for (int ep = 0; ep < 3; ++ep) if (pending[i].eps[ep]) {
            zigbee_io_operation_t config = {
                .type = ZIGBEE_IO_CONFIG_REPORT,
                .short_addr = pending[i].addr,
                .endpoint = pending[i].eps[ep],
                .due_ms = due_ms + (int64_t)ep * APP_ZIGBEE_READ_STAGGER_MS,
            };
            queue_operation(&config);
        }
        due_ms += 3 * APP_ZIGBEE_READ_STAGGER_MS;
    }
#if LIGHTGW_HAS_DISCOVERY
    for (size_t i = 0; i < rediscover_count; ++i) {
        ESP_LOGI(TAG, "DIAG rediscover persisted unmapped device 0x%04x after network restore",
                 rediscover[i].addr);
        discover_device(rediscover[i].addr, rediscover[i].ieee);
    }
#endif
}

static void queue_due_recovery_polls(void)
{
    struct { uint16_t addr; esp_zb_ieee_addr_t ieee; uint8_t eps[3]; uint8_t attempt; } pending[APP_MAX_ZIGBEE_DEVICES];
    size_t count = 0;
    int64_t now = now_ms();
    xSemaphoreTake(s_devices_lock, portMAX_DELAY);
    for (size_t i = 0; i < APP_MAX_ZIGBEE_DEVICES; ++i) {
        uint8_t attempt = 0;
        if (!s_devices[i].used || !s_devices[i].supported || !s_devices[i].mapping_confirmed) continue;
        if (!zigbee_recovery_take_due(&s_recovery[i], now, s_last_state_seen_ms[i], &attempt)) continue;
        pending[count].addr = s_devices[i].short_addr;
        memcpy(pending[count].ieee, s_devices[i].ieee_addr, sizeof(esp_zb_ieee_addr_t));
        memcpy(pending[count].eps, s_devices[i].endpoints, sizeof(pending[count].eps));
        pending[count].attempt = attempt;
        count++;
    }
    xSemaphoreGive(s_devices_lock);

    for (size_t i = 0; i < count; ++i) {
        pending[i].addr = resolve_short_address(pending[i].ieee, pending[i].addr, "recovery");
        ESP_LOGI(TAG, "Recovery probe %u/%u short=0x%04x",
                 pending[i].attempt + 1, ZIGBEE_RECOVERY_MAX_ATTEMPTS, pending[i].addr);
        queue_all_states(pending[i].addr, pending[i].eps, now);
    }
}

static void send_config_report(uint16_t addr, uint8_t endpoint)
{
    bool change = false;
    esp_zb_zcl_config_report_record_t record = {
        .direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND,
        .attributeID = ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
        .attrType = ESP_ZB_ZCL_ATTR_TYPE_BOOL,
        .min_interval = 1,
        .max_interval = 300,
        .reportable_change = &change,
    };
    esp_zb_zcl_config_report_cmd_t report = {0};
    report.zcl_basic_cmd.dst_addr_u.addr_short = addr;
    report.zcl_basic_cmd.dst_endpoint = endpoint;
    report.zcl_basic_cmd.src_endpoint = APP_GATEWAY_ENDPOINT;
    report.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    report.clusterID = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF;
    report.record_number = 1;
    report.record_field = &record;
    uint8_t tsn = esp_zb_zcl_config_report_cmd_req(&report);
    ESP_LOGI(TAG, "ZCL reporting config TX tsn=%u short=0x%04x endpoint=%u", tsn, addr, endpoint);
}

static bool control_already_confirmed(const zigbee_io_operation_t *operation)
{
    bool confirmed = false;
    xSemaphoreTake(s_devices_lock, portMAX_DELAY);
    zb_state_cache_entry_t cached;
    if (zb_state_cache_get(&s_state_cache, operation->short_addr, operation->endpoint,
                           now_ms(), &cached) && cached.last_known_valid &&
        cached.last_known_updated_ms >= operation->origin_ms &&
        cached.last_known_value == (operation->requested_on ? ZB_CACHE_ON : ZB_CACHE_OFF)) {
        confirmed = true;
    }
    xSemaphoreGive(s_devices_lock);
    return confirmed;
}

static bool control_is_current(const zigbee_io_operation_t *operation)
{
    bool current = false;
    xSemaphoreTake(s_devices_lock, portMAX_DELAY);
    int idx = find_device_by_short(operation->short_addr);
    if (idx >= 0) {
        for (int channel = 0; channel < 3; ++channel) {
            if (s_devices[idx].endpoints[channel] == operation->endpoint &&
                s_control_generation[idx][channel] == operation->control_generation) {
                current = true;
                break;
            }
        }
    }
    xSemaphoreGive(s_devices_lock);
    return current;
}

static void execute_io_operation(const zigbee_io_operation_t *operation)
{
    if (!s_network_ready) return;
    if (operation->type == ZIGBEE_IO_CONTROL && !control_is_current(operation)) {
        ESP_LOGI(TAG, "Superseded control skipped short=0x%04x endpoint=%u generation=%lu",
                 operation->short_addr, operation->endpoint,
                 (unsigned long)operation->control_generation);
        return;
    }
    if (operation->type == ZIGBEE_IO_CONTROL && operation->attempt > 0 &&
        control_already_confirmed(operation)) {
        ESP_LOGI(TAG, "Control retry skipped: fresh state already confirmed short=0x%04x endpoint=%u",
                 operation->short_addr, operation->endpoint);
        return;
    }

    if (operation->type == ZIGBEE_IO_READ) {
        if (operation->cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
            operation->attribute == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
            xSemaphoreTake(s_devices_lock, portMAX_DELAY);
            zb_state_cache_begin_read(&s_state_cache, operation->short_addr, operation->endpoint, now_ms());
            xSemaphoreGive(s_devices_lock);
        }
        esp_zb_lock_acquire(portMAX_DELAY);
        send_read(operation->short_addr, operation->endpoint, operation->cluster, operation->attribute);
        esp_zb_lock_release();
        return;
    }

    if (operation->type == ZIGBEE_IO_CONFIG_REPORT) {
        esp_zb_lock_acquire(portMAX_DELAY);
        send_config_report(operation->short_addr, operation->endpoint);
        esp_zb_lock_release();
        return;
    }

    esp_zb_zcl_on_off_cmd_t command = {0};
    command.zcl_basic_cmd.dst_addr_u.addr_short = operation->short_addr;
    command.zcl_basic_cmd.dst_endpoint = operation->endpoint;
    command.zcl_basic_cmd.src_endpoint = APP_GATEWAY_ENDPOINT;
    command.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    command.on_off_cmd_id = operation->requested_on ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID;
    esp_zb_lock_acquire(portMAX_DELAY);
    uint8_t tsn = esp_zb_zcl_on_off_cmd_req(&command);
    esp_zb_lock_release();
    ESP_LOGI(TAG, "Control TX tsn=%u short=0x%04x endpoint=%u value=%s attempt=%u/%u",
             tsn, operation->short_addr, operation->endpoint,
             operation->requested_on ? "ON" : "OFF", operation->attempt + 1,
             ZIGBEE_CONTROL_RETRY_ATTEMPTS);

    queue_read(operation->short_addr, operation->endpoint, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
               ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
               now_ms() + APP_ZIGBEE_CONTROL_READ_DELAY_MS);
    if (zigbee_control_retry_has_next(operation->attempt)) {
        zigbee_io_operation_t retry = *operation;
        retry.attempt++;
        retry.due_ms = retry.origin_ms + zigbee_control_retry_offset_ms(retry.attempt);
        queue_operation(&retry);
    }
}

static void queue_background_poll(void)
{
    struct { uint16_t addr; esp_zb_ieee_addr_t ieee; uint8_t eps[3]; } pending[APP_MAX_ZIGBEE_DEVICES];
    struct { uint16_t addr; esp_zb_ieee_addr_t ieee; } routers[APP_MAX_ZIGBEE_DEVICES];
    size_t count = 0;
    size_t router_count = 0;
    xSemaphoreTake(s_devices_lock, portMAX_DELAY);
    for (size_t i = 0; i < APP_MAX_ZIGBEE_DEVICES; ++i) {
        if (!s_devices[i].used) continue;
        if (strcmp(s_devices[i].model, LIGHTGW_ROUTER_MODEL_ID) == 0 ||
            strcmp(s_devices[i].model, LIGHTGW_ROUTER_LEGACY_MODEL_ID) == 0) {
            routers[router_count].addr = s_devices[i].short_addr;
            memcpy(routers[router_count].ieee, s_devices[i].ieee_addr,
                   sizeof(esp_zb_ieee_addr_t));
            router_count++;
        } else if (s_devices[i].supported && s_devices[i].mapping_confirmed) {
            pending[count].addr = s_devices[i].short_addr;
            memcpy(pending[count].ieee, s_devices[i].ieee_addr, sizeof(esp_zb_ieee_addr_t));
            memcpy(pending[count].eps, s_devices[i].endpoints, sizeof(pending[count].eps));
            count++;
        }
    }
    xSemaphoreGive(s_devices_lock);
    int64_t due_ms = now_ms();
    for (size_t i = 0; i < count; ++i) {
        pending[i].addr = resolve_short_address(pending[i].ieee, pending[i].addr, "background-poll");
        queue_all_states(pending[i].addr, pending[i].eps, due_ms);
        due_ms += 3 * APP_ZIGBEE_READ_STAGGER_MS;
    }
    for (size_t i = 0; i < router_count; ++i) {
        routers[i].addr = resolve_short_address(routers[i].ieee, routers[i].addr,
                                                "router-heartbeat");
        queue_read(routers[i].addr, 1, ESP_ZB_ZCL_CLUSTER_ID_BASIC,
                   ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, due_ms);
        due_ms += APP_ZIGBEE_READ_STAGGER_MS;
    }
}

static void zigbee_io_task(void *arg)
{
    (void)arg;
    size_t pending_count = 0;
    int64_t next_poll_ms = now_ms() + APP_ZIGBEE_STATE_POLL_MS;
    int64_t next_recovery_check_ms = now_ms();
    for (;;) {
        zigbee_io_operation_t incoming;
        while (pending_count < ZIGBEE_IO_PENDING_CAPACITY &&
               xQueueReceive(s_zigbee_io_queue, &incoming, 0) == pdTRUE) {
            s_pending_operations[pending_count++] = incoming;
        }
        int64_t now = now_ms();
        if (s_network_ready && now >= next_recovery_check_ms) {
            queue_due_recovery_polls();
            next_recovery_check_ms = now + 250;
        }
        if (s_network_ready && now >= next_poll_ms) {
            queue_background_poll();
            next_poll_ms = now + APP_ZIGBEE_STATE_POLL_MS;
        }
        size_t due_index = SIZE_MAX;
        int64_t earliest = INT64_MAX;
        for (size_t i = 0; i < pending_count; ++i) {
            if (s_pending_operations[i].due_ms < earliest) {
                earliest = s_pending_operations[i].due_ms;
                due_index = i;
            }
        }
        if (due_index != SIZE_MAX && earliest <= now) {
            zigbee_io_operation_t operation = s_pending_operations[due_index];
            s_pending_operations[due_index] = s_pending_operations[--pending_count];
            execute_io_operation(&operation);
            continue;
        }
        if (pending_count >= ZIGBEE_IO_PENDING_CAPACITY) {
            ESP_LOGW(TAG, "Zigbee pending operation capacity reached");
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

#if LIGHTGW_HAS_DISCOVERY
static void request_next_simple_desc(discovery_ctx_t *ctx);

static bool begin_discovery(uint16_t short_addr)
{
    for (size_t i = 0; i < APP_MAX_ZIGBEE_DEVICES; ++i) {
        if (s_discovery_in_progress[i] == short_addr) return false;
    }
    for (size_t i = 0; i < APP_MAX_ZIGBEE_DEVICES; ++i) {
        if (s_discovery_in_progress[i] == 0) {
            s_discovery_in_progress[i] = short_addr;
            return true;
        }
    }
    return false;
}

static void end_discovery(uint16_t short_addr)
{
    for (size_t i = 0; i < APP_MAX_ZIGBEE_DEVICES; ++i) {
        if (s_discovery_in_progress[i] == short_addr) {
            s_discovery_in_progress[i] = 0;
            return;
        }
    }
}

static void simple_desc_cb(esp_zb_zdp_status_t status, esp_zb_af_simple_desc_1_1_t *desc, void *user_ctx)
{
    discovery_ctx_t *ctx = user_ctx;
    if (status == ESP_ZB_ZDP_STATUS_SUCCESS && desc && desc->app_profile_id == ESP_ZB_AF_HA_PROFILE_ID) {
        ESP_LOGI(TAG, "DIAG short=0x%04x ep=%u profile=0x%04x device=0x%04x in_clusters=%u out_clusters=%u",
                 ctx->short_addr, desc->endpoint, desc->app_profile_id, desc->app_device_id,
                 desc->app_input_cluster_count, desc->app_output_cluster_count);
        for (uint8_t i = 0; i < desc->app_input_cluster_count; ++i) {
            ESP_LOGI(TAG, "DIAG short=0x%04x ep=%u input_cluster=0x%04x",
                     ctx->short_addr, desc->endpoint, desc->app_cluster_list[i]);
            if (desc->app_cluster_list[i] == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF && ctx->onoff_count < APP_MAX_ZIGBEE_ENDPOINTS) {
                ctx->onoff_eps[ctx->onoff_count++] = desc->endpoint;
                ESP_LOGI(TAG, "0x%04x endpoint %u exposes On/Off server cluster", ctx->short_addr, desc->endpoint);
                break;
            }
        }
        for (uint8_t i = 0; i < desc->app_output_cluster_count; ++i) {
            ESP_LOGI(TAG, "DIAG short=0x%04x ep=%u output_cluster=0x%04x",
                     ctx->short_addr, desc->endpoint,
                     desc->app_cluster_list[desc->app_input_cluster_count + i]);
        }
    } else {
        ESP_LOGW(TAG, "DIAG simple descriptor failed short=0x%04x status=%u",
                 ctx->short_addr, status);
    }
    request_next_simple_desc(ctx);
}

static bool merge_endpoint(uint8_t *endpoints, uint8_t *count, uint8_t endpoint)
{
    for (uint8_t i = 0; i < *count; ++i) {
        if (endpoints[i] == endpoint) return false;
    }
    if (*count >= APP_MAX_ZIGBEE_ENDPOINTS) return false;
    endpoints[(*count)++] = endpoint;
    return true;
}

static void finish_discovery(discovery_ctx_t *ctx)
{
    uint8_t mapped_endpoints[3] = {0};
    bool mapping_was_confirmed = false;
    xSemaphoreTake(s_devices_lock, portMAX_DELAY);
    int idx = find_or_add_device(ctx->short_addr, ctx->ieee);
    if (idx >= 0) {
        zb_device_snapshot_t *device = &s_devices[idx];
        bool changed = false;
        for (uint8_t i = 0; i < ctx->ep_count; ++i) {
            changed |= merge_endpoint(device->discovered_endpoints,
                                      &device->discovered_endpoint_count, ctx->eps[i]);
        }
        for (uint8_t i = 0; i < ctx->onoff_count; ++i) {
            changed |= merge_endpoint(device->onoff_endpoints,
                                      &device->onoff_endpoint_count, ctx->onoff_eps[i]);
            zb_state_cache_register_endpoint(&s_state_cache, ctx->ieee, ctx->short_addr,
                                             ctx->onoff_eps[i]);
        }
        if (changed) save_devices();
        if (ctx->onoff_count == 0) {
            ESP_LOGW(TAG, "DIAG empty discovery ignored for 0x%04x; previous valid endpoints preserved",
                     ctx->short_addr);
        } else if (ctx->onoff_count < device->onoff_endpoint_count) {
            ESP_LOGW(TAG, "DIAG partial discovery merged for 0x%04x new=%u retained_total=%u",
                     ctx->short_addr, ctx->onoff_count, device->onoff_endpoint_count);
        }
        mapping_was_confirmed = device->mapping_confirmed;
        if (mapping_was_confirmed) memcpy(mapped_endpoints, device->endpoints, sizeof(mapped_endpoints));
    }
    xSemaphoreGive(s_devices_lock);
    if (mapping_was_confirmed) {
        int64_t due_ms = now_ms() + 1000;
        for (int ch = 0; ch < 3; ++ch) if (mapped_endpoints[ch]) {
            zigbee_io_operation_t config = {
                .type = ZIGBEE_IO_CONFIG_REPORT,
                .short_addr = ctx->short_addr,
                .endpoint = mapped_endpoints[ch],
                .due_ms = due_ms + (int64_t)ch * APP_ZIGBEE_READ_STAGGER_MS,
            };
            queue_operation(&config);
        }
        queue_all_states(ctx->short_addr, mapped_endpoints,
                         due_ms + 3 * APP_ZIGBEE_READ_STAGGER_MS);
    }
    queue_read(ctx->short_addr, 1, ESP_ZB_ZCL_CLUSTER_ID_BASIC,
               ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, now_ms());
    diagnostics_record_memory_event("zigbee_discovery_done");
    end_discovery(ctx->short_addr);
    free(ctx);
}

static void request_next_simple_desc(discovery_ctx_t *ctx)
{
    if (ctx->next_ep >= ctx->ep_count) {
        finish_discovery(ctx);
        return;
    }
    esp_zb_zdo_simple_desc_req_param_t req = {
        .addr_of_interest = ctx->short_addr,
        .endpoint = ctx->eps[ctx->next_ep++],
    };
    esp_zb_zdo_simple_desc_req(&req, simple_desc_cb, ctx);
}

static void active_ep_cb(esp_zb_zdp_status_t status, uint8_t count, uint8_t *eps, void *user_ctx)
{
    discovery_ctx_t *ctx = user_ctx;
    if (status != ESP_ZB_ZDP_STATUS_SUCCESS || !eps) {
        ESP_LOGW(TAG, "Active endpoint discovery failed for 0x%04x", ctx->short_addr);
        diagnostics_record_memory_event("zigbee_discovery_failed");
        end_discovery(ctx->short_addr);
        free(ctx);
        return;
    }
    ctx->ep_count = count > APP_MAX_ZIGBEE_ENDPOINTS ? APP_MAX_ZIGBEE_ENDPOINTS : count;
    memcpy(ctx->eps, eps, ctx->ep_count);
    for (uint8_t i = 0; i < ctx->ep_count; ++i) {
        ESP_LOGI(TAG, "DIAG short=0x%04x active_endpoint=%u", ctx->short_addr, ctx->eps[i]);
    }
    request_next_simple_desc(ctx);
}

static void discover_device(uint16_t short_addr, const esp_zb_ieee_addr_t ieee)
{
    if (!begin_discovery(short_addr)) {
        ESP_LOGI(TAG, "DIAG discovery already running for 0x%04x; duplicate announce ignored",
                 short_addr);
        return;
    }
    discovery_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        end_discovery(short_addr);
        diagnostics_set_error(TAG, "cannot allocate discovery context for 0x%04x", short_addr);
        return;
    }
    ctx->short_addr = short_addr;
    memcpy(ctx->ieee, ieee, sizeof(ctx->ieee));
    esp_zb_zdo_active_ep_req_param_t req = {.addr_of_interest = short_addr};
    esp_zb_zdo_active_ep_req(&req, active_ep_cb, ctx);
}
#endif

static void update_onoff(uint16_t addr, uint8_t endpoint, bool on, int8_t rssi)
{
    bool mapped = false;
    xSemaphoreTake(s_devices_lock, portMAX_DELAY);
    int idx = find_device_by_short(addr);
    if (idx >= 0) {
        bool cache_updated = zb_state_cache_update(&s_state_cache, addr, endpoint,
                                                   on ? ZB_CACHE_ON : ZB_CACHE_OFF, now_ms());
        for (int ch = 0; ch < 3; ++ch) if (s_devices[idx].endpoints[ch] == endpoint) {
            s_devices[idx].state[ch] = cache_updated ? (on ? ZB_CHANNEL_ON : ZB_CHANNEL_OFF) : ZB_CHANNEL_UNKNOWN;
            mapped = true;
        }
        s_devices[idx].online = true;
        s_last_seen_ms[idx] = now_ms();
        s_last_state_seen_ms[idx] = s_last_seen_ms[idx];
        zigbee_recovery_note_response(&s_recovery[idx], s_last_state_seen_ms[idx]);
        if (rssi != 0) s_devices[idx].last_rssi = rssi;
    }
    xSemaphoreGive(s_devices_lock);
    ESP_LOGI(TAG, "State feedback short=0x%04x endpoint=%u value=%s rssi=%d mapped=%s",
             addr, endpoint, on ? "ON" : "OFF", rssi, mapped ? "yes" : "no");
}

static void handle_read_response(const esp_zb_zcl_cmd_read_attr_resp_message_t *msg)
{
    if (!msg) return;
    if (msg->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "ZCL read response failed short=0x%04x endpoint=%u cluster=0x%04x status=0x%02x tsn=%u",
                 msg->info.src_address.u.short_addr, msg->info.src_endpoint, msg->info.cluster,
                 msg->info.status, msg->info.header.tsn);
        return;
    }
    for (esp_zb_zcl_read_attr_resp_variable_t *v = msg->variables; v; v = v->next) {
        if (v->status != ESP_ZB_ZCL_STATUS_SUCCESS || !v->attribute.data.value) {
            ESP_LOGW(TAG, "ZCL attribute read rejected short=0x%04x endpoint=%u cluster=0x%04x attr=0x%04x status=0x%02x",
                     msg->info.src_address.u.short_addr, msg->info.src_endpoint, msg->info.cluster,
                     v->attribute.id, v->status);
            continue;
        }
        if (msg->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF && v->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
            update_onoff(msg->info.src_address.u.short_addr, msg->info.src_endpoint,
                         *(uint8_t *)v->attribute.data.value != 0, msg->info.header.rssi);
        } else if (msg->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_BASIC && v->attribute.id == ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID) {
            uint8_t *zcl = v->attribute.data.value;
            size_t len = zcl[0] < 32 ? zcl[0] : 32;
            char model[33] = {0};
            memcpy(model, zcl + 1, len);
            xSemaphoreTake(s_devices_lock, portMAX_DELAY);
            int idx = find_device_by_short(msg->info.src_address.u.short_addr);
            if (idx >= 0) {
                bool model_changed = strcmp(s_devices[idx].model, model) != 0;
                bool supported_before = s_devices[idx].supported;
                strlcpy(s_devices[idx].model, model, sizeof(s_devices[idx].model));
                s_devices[idx].supported = strcmp(model, AQARA_MODEL_ID) == 0;
                s_devices[idx].online = true;
                s_last_seen_ms[idx] = now_ms();
                if (msg->info.header.rssi != 0) s_devices[idx].last_rssi = msg->info.header.rssi;
                if (s_devices[idx].supported) {
                    if (model_changed || !supported_before) save_devices();
                    xSemaphoreGive(s_devices_lock);
                    ESP_LOGW(TAG, "QBKG25LM model found at 0x%04x; control remains locked until physical channel mapping is confirmed",
                             msg->info.src_address.u.short_addr);
                    return;
                }
                if (strcmp(model, LIGHTGW_ROUTER_MODEL_ID) == 0 ||
                    strcmp(model, LIGHTGW_ROUTER_LEGACY_MODEL_ID) == 0) {
                    ESP_LOGI(TAG, "Zigbee Router heartbeat short=0x%04x model='%s' rssi=%d",
                             msg->info.src_address.u.short_addr, model, msg->info.header.rssi);
                } else {
                    ESP_LOGW(TAG, "Unsupported device 0x%04x model='%s'",
                             msg->info.src_address.u.short_addr, model);
                }
                if (model_changed || supported_before) save_devices();
            }
            xSemaphoreGive(s_devices_lock);
        }
    }
}

static esp_err_t action_handler(esp_zb_core_action_callback_id_t id, const void *message)
{
    if (id == ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID) {
        handle_read_response(message);
    } else if (id == ESP_ZB_CORE_REPORT_ATTR_CB_ID) {
        const esp_zb_zcl_report_attr_message_t *m = message;
        if (m && m->status == ESP_ZB_ZCL_STATUS_SUCCESS && m->cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
            m->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID && m->attribute.data.value) {
            update_onoff(m->src_address.u.short_addr, m->src_endpoint,
                         *(uint8_t *)m->attribute.data.value != 0, 0);
        }
    } else if (id == ESP_ZB_CORE_CMD_REPORT_CONFIG_RESP_CB_ID) {
        const esp_zb_zcl_cmd_config_report_resp_message_t *m = message;
        if (m) ESP_LOGI(TAG, "ZCL reporting config response short=0x%04x endpoint=%u status=0x%02x tsn=%u",
                        m->info.src_address.u.short_addr, m->info.src_endpoint,
                        m->info.status, m->info.header.tsn);
    } else if (id == ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID) {
        const esp_zb_zcl_cmd_default_resp_message_t *m = message;
        if (m) ESP_LOGI(TAG, "ZCL default response short=0x%04x endpoint=%u cluster=0x%04x command=0x%02x status=0x%02x tsn=%u",
                        m->info.src_address.u.short_addr, m->info.src_endpoint, m->info.cluster,
                        m->resp_to_cmd, m->status_code, m->info.header.tsn);
    }
    return ESP_OK;
}

static void command_send_status_handler(esp_zb_zcl_command_send_status_message_t message)
{
    if (message.dst_addr.addr_type == ESP_ZB_ZCL_ADDR_TYPE_SHORT) {
        ESP_LOGI(TAG, "ZCL send status tsn=%u short=0x%04x endpoint=%u status=%s",
                 message.tsn, message.dst_addr.u.short_addr, message.dst_endpoint,
                 esp_err_to_name(message.status));
    } else {
        ESP_LOGI(TAG, "ZCL send status tsn=%u endpoint=%u status=%s",
                 message.tsn, message.dst_endpoint, esp_err_to_name(message.status));
    }
}

static void commissioning_retry(uint8_t mode)
{
    esp_zb_bdb_start_top_level_commissioning(mode);
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal)
{
    uint32_t *p = signal->p_app_signal;
    esp_zb_app_signal_type_t type = *p;
    esp_err_t status = signal->esp_err_status;
    switch (type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        if (esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION) != ESP_OK) {
            diagnostics_set_error(TAG, "failed to start Zigbee initialization");
        }
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (status == ESP_OK) {
            if (esp_zb_bdb_is_factory_new()) {
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                s_network_ready = true;
                ESP_LOGI(TAG, "Restored Zigbee network PAN=0x%04x channel=%u", esp_zb_get_pan_id(), esp_zb_get_current_channel());
                diagnostics_record_memory_event("zigbee_restored");
                poll_persisted_devices();
            }
        } else {
            diagnostics_set_error(TAG, "initialization failed: %s", esp_err_to_name(status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)commissioning_retry, ESP_ZB_BDB_MODE_INITIALIZATION, 1000);
        }
        break;
    case ESP_ZB_BDB_SIGNAL_FORMATION:
        if (status == ESP_OK) {
            s_network_ready = true;
            ESP_LOGI(TAG, "Formed Zigbee network PAN=0x%04x channel=%u", esp_zb_get_pan_id(), esp_zb_get_current_channel());
            diagnostics_record_memory_event("zigbee_formed");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        } else {
            diagnostics_set_error(TAG, "network formation failed: %s", esp_err_to_name(status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)commissioning_retry, ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
        break;
    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        esp_zb_zdo_signal_device_annce_params_t *d = esp_zb_app_signal_get_params(p);
        ESP_LOGI(TAG, "Device announce short=0x%04x", d->device_short_addr);
        diagnostics_record_memory_event("zigbee_device_announce");
#if LIGHTGW_HAS_DISCOVERY
        xSemaphoreTake(s_devices_lock, portMAX_DELAY);
        zb_state_cache_device_announced(&s_state_cache, d->ieee_addr, d->device_short_addr, now_ms());
        int prior = find_device_by_ieee(d->ieee_addr);
        if (prior >= 0) {
            s_devices[prior].short_addr = d->device_short_addr;
            s_devices[prior].online = true;
            s_last_seen_ms[prior] = now_ms();
            s_devices[prior].state[0] = s_devices[prior].state[1] = s_devices[prior].state[2] = ZB_CHANNEL_UNKNOWN;
            zigbee_recovery_begin(&s_recovery[prior], now_ms());
            save_devices();
        }
        xSemaphoreGive(s_devices_lock);
        discover_device(d->device_short_addr, d->ieee_addr);
#else
        ESP_LOGI(TAG, "Stage 2 records join only; endpoint discovery is enabled in Stage 3");
#endif
        break;
    }
    case ESP_ZB_ZDO_SIGNAL_DEVICE_UPDATE: {
        esp_zb_zdo_signal_device_update_params_t *d = esp_zb_app_signal_get_params(p);
        if (!d) break;
        ESP_LOGI(TAG, "Device update short=0x%04x status=%u tc_action=%u parent=0x%04x",
                 d->short_addr, d->status, d->tc_action, d->parent_short);
#if LIGHTGW_HAS_DISCOVERY
        xSemaphoreTake(s_devices_lock, portMAX_DELAY);
        int prior = find_device_by_ieee(d->long_addr);
        if (prior >= 0) {
            if (d->status == ESP_ZB_ZDO_STANDARD_DEV_DEVICE_LEFT ||
                d->tc_action == ESP_ZB_ZDO_TC_ACTION_DENY) {
                s_devices[prior].online = false;
                s_last_seen_ms[prior] = 0;
                s_devices[prior].state[0] = s_devices[prior].state[1] = s_devices[prior].state[2] = ZB_CHANNEL_UNKNOWN;
                s_recovery[prior].active = false;
            } else if (d->tc_action == ESP_ZB_ZDO_TC_ACTION_AUTHORIZE) {
                bool address_changed = s_devices[prior].short_addr != d->short_addr;
                s_devices[prior].short_addr = d->short_addr;
                s_devices[prior].online = true;
                s_last_seen_ms[prior] = now_ms();
                s_devices[prior].state[0] = s_devices[prior].state[1] = s_devices[prior].state[2] = ZB_CHANNEL_UNKNOWN;
                zb_state_cache_device_announced(&s_state_cache, d->long_addr, d->short_addr, now_ms());
                zigbee_recovery_begin(&s_recovery[prior], now_ms());
                if (address_changed) save_devices();
            }
        }
        xSemaphoreGive(s_devices_lock);
#else
        ESP_LOGI(TAG, "Stage 2 records device update only; persisted mapping recovery starts in Stage 3");
#endif
        break;
    }
    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        ESP_LOGI(TAG, "Permit join status=%u", *(uint8_t *)esp_zb_app_signal_get_params(p));
        diagnostics_record_memory_event("zigbee_permit_status");
        break;
    default:
        ESP_LOGD(TAG, "Zigbee signal %s status=%s", esp_zb_zdo_signal_to_string(type), esp_err_to_name(status));
        break;
    }
}

static void zigbee_task(void *arg)
{
    (void)arg;
    const esp_zigbee_config_t cfg = {
        .device_config = {
            .device_type = EZB_NWK_DEVICE_TYPE_COORDINATOR,
            .install_code_policy = false,
            .zczr_config = {.max_children = APP_COORDINATOR_MAX_CHILDREN},
        },
        .platform_config = {
            .storage_partition_name = APP_ZIGBEE_STORAGE_PARTITION,
            .radio_config = {.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE},
        },
    };
    ESP_ERROR_CHECK(esp_zigbee_init(&cfg));
    /* This permits a previously joined legacy device to authenticate with the
       well-known key during rejoin. It does not open the network to new joins. */
    ezb_secur_tcpol_set_allow_rejoins_with_well_known_key(true);
    /* QBKG25LM is a no-neutral legacy child and can be quiet for long periods.
       Do not age it out merely because it has not sent a keepalive recently. */
    ezb_nwk_set_ed_timeout(EZB_NWK_ED_TIMEOUT_16384MIN);
    ezb_nwk_set_tx_power(APP_ZIGBEE_TX_POWER_DBM);
    ESP_LOGI(TAG, "Trust Center legacy rejoin enabled; child timeout=16384min tx_power=%ddBm",
             APP_ZIGBEE_TX_POWER_DBM);
    esp_zb_set_primary_network_channel_set(APP_ZIGBEE_CHANNEL_MASK);

    esp_zb_cluster_list_t *clusters = esp_zb_zcl_cluster_list_create();
    esp_zb_cluster_list_add_basic_cluster(clusters, esp_zb_basic_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(clusters, esp_zb_identify_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_on_off_cluster(clusters, esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
    esp_zb_cluster_list_add_basic_cluster(clusters, esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_BASIC), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
    esp_zb_cluster_list_add_custom_cluster(clusters, esp_zb_zcl_attr_list_create(AQARA_CLUSTER_LUMI), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    esp_zb_ep_list_t *eps = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = APP_GATEWAY_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_REMOTE_CONTROL_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(eps, clusters, ep_cfg);
    esp_zb_device_register(eps);
    esp_zb_core_action_handler_register(action_handler);
    esp_zb_zcl_command_send_status_handler_register(command_send_status_handler);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
    vTaskDelete(NULL);
}

esp_err_t zigbee_manager_start(void)
{
    s_devices_lock = xSemaphoreCreateMutex();
    if (!s_devices_lock) return ESP_ERR_NO_MEM;
    s_zigbee_io_queue = xQueueCreate(ZIGBEE_IO_QUEUE_DEPTH, sizeof(zigbee_io_operation_t));
    if (!s_zigbee_io_queue) return ESP_ERR_NO_MEM;
    zb_state_cache_init(&s_state_cache, APP_ZIGBEE_STATE_MAX_AGE_MS, APP_ZIGBEE_READ_TIMEOUT_MS);
    load_devices();
    esp_coex_wifi_i154_enable();
    diagnostics_record_memory_event("zigbee_starting");
    if (xTaskCreate(zigbee_task, "zigbee_main", 8192, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(zigbee_io_task, "zigbee_io", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool zigbee_manager_network_ready(void) { return s_network_ready; }
uint8_t zigbee_manager_channel(void) { return s_network_ready ? esp_zb_get_current_channel() : 0; }
uint16_t zigbee_manager_pan_id(void) { return s_network_ready ? esp_zb_get_pan_id() : 0xffff; }

int zigbee_manager_pairing_remaining(void)
{
    int64_t left = s_pairing_until_us - esp_timer_get_time();
    return left > 0 ? (int)((left + 999999) / 1000000) : 0;
}

esp_err_t zigbee_manager_permit_join(uint8_t seconds)
{
    if (!s_network_ready || seconds == 0) return ESP_ERR_INVALID_STATE;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t err = esp_zb_bdb_open_network(seconds);
    esp_zb_lock_release();
    if (err == ESP_OK) {
        s_pairing_until_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
        ESP_LOGI(TAG, "Permit join requested for %u seconds", seconds);
        diagnostics_record_memory_event("zigbee_permit_join");
    } else {
        diagnostics_set_error(TAG, "permit join failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t zigbee_manager_diagnostic_read(uint16_t short_addr)
{
    if (!s_network_ready || !s_devices_lock) return ESP_ERR_INVALID_STATE;
    uint8_t endpoints[APP_MAX_ZIGBEE_ENDPOINTS] = {0};
    esp_zb_ieee_addr_t ieee = {0};
    uint8_t count = 0;
    xSemaphoreTake(s_devices_lock, portMAX_DELAY);
    int idx = find_device_by_short(short_addr);
    if (idx >= 0) {
        memcpy(ieee, s_devices[idx].ieee_addr, sizeof(ieee));
        count = s_devices[idx].onoff_endpoint_count;
        memcpy(endpoints, s_devices[idx].onoff_endpoints, count);
    }
    xSemaphoreGive(s_devices_lock);
    if (!count) return ESP_ERR_NOT_FOUND;

    short_addr = resolve_short_address(ieee, short_addr, "manual-refresh");
    xSemaphoreTake(s_devices_lock, portMAX_DELAY);
    for (uint8_t i = 0; i < count; ++i) {
        zb_state_cache_begin_read(&s_state_cache, short_addr, endpoints[i], now_ms());
    }
    xSemaphoreGive(s_devices_lock);

    int64_t due_ms = now_ms();
    for (uint8_t i = 0; i < count; ++i) {
        queue_read(short_addr, endpoints[i], ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                   ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                   due_ms + (int64_t)i * APP_ZIGBEE_READ_STAGGER_MS);
    }
    ESP_LOGI(TAG, "DIAG read-only state refresh short=0x%04x endpoints=%u (no control command)",
             short_addr, count);
    return ESP_OK;
}

esp_err_t zigbee_manager_confirm_mapping(uint16_t short_addr, const uint8_t endpoints[3])
{
#if !LIGHTGW_HAS_CONTROL
    (void)short_addr;
    (void)endpoints;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!endpoints || endpoints[0] == endpoints[1] || endpoints[0] == endpoints[2] || endpoints[1] == endpoints[2]) {
        return ESP_ERR_INVALID_ARG;
    }
    bool valid = true;
    xSemaphoreTake(s_devices_lock, portMAX_DELAY);
    int idx = find_device_by_short(short_addr);
    if (idx < 0 || !s_devices[idx].supported) {
        valid = false;
    } else {
        for (int ch = 0; ch < 3; ++ch) {
            bool found = false;
            for (int i = 0; i < s_devices[idx].onoff_endpoint_count; ++i) {
                if (s_devices[idx].onoff_endpoints[i] == endpoints[ch]) found = true;
            }
            if (!found) valid = false;
        }
    }
    if (valid) {
        memcpy(s_devices[idx].endpoints, endpoints, 3);
        s_devices[idx].mapping_confirmed = true;
        for (int ch = 0; ch < 3; ++ch) {
            zb_state_cache_register_endpoint(&s_state_cache, s_devices[idx].ieee_addr,
                                             s_devices[idx].short_addr, endpoints[ch]);
        }
        save_devices();
    }
    xSemaphoreGive(s_devices_lock);
    if (!valid) return ESP_ERR_INVALID_ARG;
    int64_t due_ms = now_ms();
    for (int ch = 0; ch < 3; ++ch) {
        zigbee_io_operation_t config = {
            .type = ZIGBEE_IO_CONFIG_REPORT,
            .short_addr = short_addr,
            .endpoint = endpoints[ch],
            .due_ms = due_ms + (int64_t)ch * APP_ZIGBEE_READ_STAGGER_MS,
        };
        queue_operation(&config);
    }
    queue_all_states(short_addr, endpoints, due_ms + 3 * APP_ZIGBEE_READ_STAGGER_MS);
    ESP_LOGW(TAG, "Physical mapping confirmed short=0x%04x left=%u center=%u right=%u",
             short_addr, endpoints[0], endpoints[1], endpoints[2]);
    return ESP_OK;
#endif
}

esp_err_t zigbee_manager_set_channel(uint16_t addr, unsigned channel, bool on)
{
    if (!s_network_ready || channel > 2) return ESP_ERR_INVALID_ARG;
    uint8_t ep = 0;
    uint32_t generation = 0;
    esp_zb_ieee_addr_t ieee = {0};
    xSemaphoreTake(s_devices_lock, portMAX_DELAY);
    int idx = find_device_by_short(addr);
    if (idx >= 0 && s_devices[idx].supported && s_devices[idx].mapping_confirmed) {
        memcpy(ieee, s_devices[idx].ieee_addr, sizeof(ieee));
        ep = s_devices[idx].endpoints[channel];
        generation = ++s_control_generation[idx][channel];
        if (generation == 0) generation = ++s_control_generation[idx][channel];
        s_devices[idx].state[channel] = ZB_CHANNEL_UNKNOWN;
    }
    xSemaphoreGive(s_devices_lock);
    if (!ep) return ESP_ERR_NOT_FOUND;
    addr = resolve_short_address(ieee, addr, "control");
    xSemaphoreTake(s_devices_lock, portMAX_DELAY);
    zb_state_cache_begin_read(&s_state_cache, addr, ep, now_ms());
    xSemaphoreGive(s_devices_lock);
    zigbee_io_operation_t operation = {
        .type = ZIGBEE_IO_CONTROL,
        .short_addr = addr,
        .endpoint = ep,
        .requested_on = on,
        .attempt = 0,
        .control_generation = generation,
        .origin_ms = now_ms(),
        .due_ms = now_ms(),
    };
    ESP_LOGI(TAG, "Control queued short=0x%04x channel=%u endpoint=%u value=%s",
             addr, channel, ep, on ? "ON" : "OFF");
    return queue_operation(&operation) ? ESP_OK : ESP_ERR_NO_MEM;
}

size_t zigbee_manager_snapshot(zb_device_snapshot_t *out, size_t capacity)
{
    if (!out || !capacity) return 0;
    size_t n = 0;
    xSemaphoreTake(s_devices_lock, portMAX_DELAY);
    for (size_t i = 0; i < APP_MAX_ZIGBEE_DEVICES && n < capacity; ++i) if (s_devices[i].used) {
        out[n] = s_devices[i];
        out[n].online = s_last_seen_ms[i] > 0 &&
                        now_ms() - s_last_seen_ms[i] <= APP_ZIGBEE_OFFLINE_AFTER_MS;
        for (int ch = 0; ch < 3; ++ch) {
            zb_state_cache_entry_t cached;
            if (!s_devices[i].endpoints[ch] ||
                !zb_state_cache_get(&s_state_cache, s_devices[i].short_addr, s_devices[i].endpoints[ch], now_ms(), &cached) ||
                !cached.valid) out[n].state[ch] = ZB_CHANNEL_UNKNOWN;
            else out[n].state[ch] = cached.value == ZB_CACHE_ON ? ZB_CHANNEL_ON : ZB_CHANNEL_OFF;
        }
        n++;
    }
    xSemaphoreGive(s_devices_lock);
    return n;
}

esp_err_t zigbee_manager_get_channel_status(uint16_t short_addr, unsigned channel,
                                            zb_channel_status_t *status)
{
    if (!status || channel > 2 || !s_devices_lock) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));
    status->state = ZB_CHANNEL_UNKNOWN;
    status->display_state = ZB_CHANNEL_UNKNOWN;
    status->display_age_us = -1;
    xSemaphoreTake(s_devices_lock, portMAX_DELAY);
    int idx = find_device_by_short(short_addr);
    if (idx < 0 || !s_devices[idx].mapping_confirmed || !s_devices[idx].endpoints[channel]) {
        xSemaphoreGive(s_devices_lock);
        return ESP_ERR_NOT_FOUND;
    }
    status->endpoint = s_devices[idx].endpoints[channel];
    zb_state_cache_entry_t cached;
    status->known_endpoint = zb_state_cache_get(&s_state_cache, short_addr, status->endpoint,
                                                now_ms(), &cached);
    if (status->known_endpoint) {
        status->valid = cached.valid;
        status->read_pending = cached.read_pending;
        status->read_timed_out = cached.read_timed_out;
        status->last_known_valid = cached.last_known_valid;
        status->last_known_state = cached.last_known_value == ZB_CACHE_ON ? ZB_CHANNEL_ON :
                                   cached.last_known_value == ZB_CACHE_OFF ? ZB_CHANNEL_OFF : ZB_CHANNEL_UNKNOWN;
        status->consecutive_failures = cached.consecutive_failures;
        status->updated_us = cached.updated_ms * 1000;
        status->last_known_updated_us = cached.last_known_updated_ms * 1000;
        if (cached.valid) status->state = cached.value == ZB_CACHE_ON ? ZB_CHANNEL_ON : ZB_CHANNEL_OFF;
        zb_cache_value_t display_value = ZB_CACHE_UNKNOWN;
        int64_t display_age_ms = -1;
        status->display_valid = zb_state_cache_get_display(
            &s_state_cache, short_addr, status->endpoint, now_ms(),
            APP_ZIGBEE_DISPLAY_HOLD_MS, &display_value, &display_age_ms);
        if (status->display_valid) {
            status->display_state = display_value == ZB_CACHE_ON ? ZB_CHANNEL_ON : ZB_CHANNEL_OFF;
            status->display_from_cache = !cached.valid;
            status->display_age_us = display_age_ms * 1000;
        }
    }
    xSemaphoreGive(s_devices_lock);
    return status->known_endpoint ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t zigbee_manager_get_endpoint_status(uint16_t short_addr, uint8_t endpoint,
                                             zb_channel_status_t *status)
{
    if (!status || !endpoint || !s_devices_lock) return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));
    status->endpoint = endpoint;
    status->state = ZB_CHANNEL_UNKNOWN;
    status->display_state = ZB_CHANNEL_UNKNOWN;
    status->display_age_us = -1;
    xSemaphoreTake(s_devices_lock, portMAX_DELAY);
    int idx = find_device_by_short(short_addr);
    bool discovered = false;
    if (idx >= 0) {
        for (uint8_t i = 0; i < s_devices[idx].onoff_endpoint_count; ++i) {
            if (s_devices[idx].onoff_endpoints[i] == endpoint) {
                discovered = true;
                break;
            }
        }
    }
    zb_state_cache_entry_t cached;
    status->known_endpoint = discovered && zb_state_cache_get(&s_state_cache, short_addr,
                                                               endpoint, now_ms(), &cached);
    if (status->known_endpoint) {
        status->valid = cached.valid;
        status->read_pending = cached.read_pending;
        status->read_timed_out = cached.read_timed_out;
        status->last_known_valid = cached.last_known_valid;
        status->last_known_state = cached.last_known_value == ZB_CACHE_ON ? ZB_CHANNEL_ON :
                                   cached.last_known_value == ZB_CACHE_OFF ? ZB_CHANNEL_OFF : ZB_CHANNEL_UNKNOWN;
        status->consecutive_failures = cached.consecutive_failures;
        status->updated_us = cached.updated_ms * 1000;
        status->last_known_updated_us = cached.last_known_updated_ms * 1000;
        if (cached.valid) status->state = cached.value == ZB_CACHE_ON ? ZB_CHANNEL_ON : ZB_CHANNEL_OFF;
        zb_cache_value_t display_value = ZB_CACHE_UNKNOWN;
        int64_t display_age_ms = -1;
        status->display_valid = zb_state_cache_get_display(
            &s_state_cache, short_addr, endpoint, now_ms(), APP_ZIGBEE_DISPLAY_HOLD_MS,
            &display_value, &display_age_ms);
        if (status->display_valid) {
            status->display_state = display_value == ZB_CACHE_ON ? ZB_CHANNEL_ON : ZB_CHANNEL_OFF;
            status->display_from_cache = !cached.valid;
            status->display_age_us = display_age_ms * 1000;
        }
    }
    xSemaphoreGive(s_devices_lock);
    return status->known_endpoint ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t zigbee_manager_get_last_seen_age(uint16_t short_addr, int64_t *age_ms)
{
    if (!age_ms || !s_devices_lock) return ESP_ERR_INVALID_ARG;
    *age_ms = -1;
    xSemaphoreTake(s_devices_lock, portMAX_DELAY);
    int idx = find_device_by_short(short_addr);
    if (idx >= 0 && s_last_seen_ms[idx] > 0) {
        int64_t age = now_ms() - s_last_seen_ms[idx];
        *age_ms = age > 0 ? age : 0;
    }
    xSemaphoreGive(s_devices_lock);
    return idx >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}
