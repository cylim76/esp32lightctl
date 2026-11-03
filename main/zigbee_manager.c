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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "stage_config.h"
#include "storage_manager.h"
#include "zigbee_state_cache.h"
#include "zcl/esp_zigbee_zcl_command.h"

static const char *TAG = "zigbee";
static SemaphoreHandle_t s_devices_lock;
static zb_device_snapshot_t s_devices[APP_MAX_ZIGBEE_DEVICES];
static zb_device_snapshot_t s_persisted_devices[APP_MAX_ZIGBEE_DEVICES];
static bool s_device_storage_healthy = true;
static bool s_network_ready;
static int64_t s_pairing_until_us;
static zb_state_cache_t s_state_cache;

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
#endif

static void send_read(uint16_t addr, uint8_t endpoint, uint16_t cluster, uint16_t attr)
{
    esp_zb_zcl_read_attr_cmd_t req = {0};
    req.zcl_basic_cmd.dst_addr_u.addr_short = addr;
    req.zcl_basic_cmd.dst_endpoint = endpoint;
    req.zcl_basic_cmd.src_endpoint = APP_GATEWAY_ENDPOINT;
    req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    req.clusterID = cluster;
    req.attr_number = 1;
    req.attr_field = &attr;
    esp_zb_zcl_read_attr_cmd_req(&req);
}

static void read_all_states(uint16_t addr, const uint8_t eps[3])
{
    for (int i = 0; i < 3; ++i) if (eps[i]) {
        xSemaphoreTake(s_devices_lock, portMAX_DELAY);
        zb_state_cache_begin_read(&s_state_cache, addr, eps[i], now_ms());
        xSemaphoreGive(s_devices_lock);
        send_read(addr, eps[i], ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
    }
}

static void poll_persisted_devices(void)
{
    struct { uint16_t addr; uint8_t eps[3]; } pending[APP_MAX_ZIGBEE_DEVICES];
    size_t count = 0;
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
        }
    }
    xSemaphoreGive(s_devices_lock);
    for (size_t i = 0; i < count; ++i) read_all_states(pending[i].addr, pending[i].eps);
}

#if LIGHTGW_HAS_CONTROL
static void configure_aqara(uint16_t addr, const uint8_t eps[3])
{
    uint8_t mode = 1;
    esp_zb_zcl_attribute_t attr = {
        .id = AQARA_ATTR_MODE,
        .data = {.type = ESP_ZB_ZCL_ATTR_TYPE_U8, .size = sizeof(mode), .value = &mode},
    };
    esp_zb_zcl_write_attr_cmd_t write = {0};
    write.zcl_basic_cmd.dst_addr_u.addr_short = addr;
    write.zcl_basic_cmd.dst_endpoint = 1;
    write.zcl_basic_cmd.src_endpoint = APP_GATEWAY_ENDPOINT;
    write.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    write.clusterID = AQARA_CLUSTER_LUMI;
    write.manuf_specific = 1;
    write.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    write.dis_default_resp = 1;
    write.manuf_code = AQARA_MANUFACTURER_CODE;
    write.attr_number = 1;
    write.attr_field = &attr;
    esp_zb_zcl_write_attr_cmd_req(&write);

    bool change = false;
    for (int i = 0; i < 3; ++i) {
        esp_zb_zcl_config_report_record_t record = {
            .direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND,
            .attributeID = ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
            .attrType = ESP_ZB_ZCL_ATTR_TYPE_BOOL,
            .min_interval = 0,
            .max_interval = 300,
            .reportable_change = &change,
        };
        esp_zb_zcl_config_report_cmd_t report = {0};
        report.zcl_basic_cmd.dst_addr_u.addr_short = addr;
        report.zcl_basic_cmd.dst_endpoint = eps[i];
        report.zcl_basic_cmd.src_endpoint = APP_GATEWAY_ENDPOINT;
        report.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
        report.clusterID = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF;
        report.record_number = 1;
        report.record_field = &record;
        esp_zb_zcl_config_report_cmd_req(&report);
    }
    read_all_states(addr, eps);
}
#endif

#if LIGHTGW_HAS_DISCOVERY
static void request_next_simple_desc(discovery_ctx_t *ctx);

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

static void finish_discovery(discovery_ctx_t *ctx)
{
    xSemaphoreTake(s_devices_lock, portMAX_DELAY);
    int idx = find_or_add_device(ctx->short_addr, ctx->ieee);
    if (idx >= 0) {
        s_devices[idx].mapping_confirmed = false;
        memset(s_devices[idx].endpoints, 0, sizeof(s_devices[idx].endpoints));
        s_devices[idx].discovered_endpoint_count = ctx->ep_count;
        memcpy(s_devices[idx].discovered_endpoints, ctx->eps, ctx->ep_count);
        s_devices[idx].onoff_endpoint_count = ctx->onoff_count;
        memcpy(s_devices[idx].onoff_endpoints, ctx->onoff_eps, ctx->onoff_count);
        for (uint8_t i = 0; i < ctx->onoff_count; ++i) {
            zb_state_cache_register_endpoint(&s_state_cache, ctx->ieee, ctx->short_addr, ctx->onoff_eps[i]);
        }
        save_devices();
    }
    xSemaphoreGive(s_devices_lock);
    send_read(ctx->short_addr, 1, ESP_ZB_ZCL_CLUSTER_ID_BASIC, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID);
    diagnostics_record_memory_event("zigbee_discovery_done");
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
    discovery_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return;
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
        s_devices[idx].last_rssi = rssi;
    }
    xSemaphoreGive(s_devices_lock);
    ESP_LOGI(TAG, "State feedback short=0x%04x endpoint=%u value=%s rssi=%d mapped=%s",
             addr, endpoint, on ? "ON" : "OFF", rssi, mapped ? "yes" : "no");
}

static void handle_read_response(const esp_zb_zcl_cmd_read_attr_resp_message_t *msg)
{
    if (!msg || msg->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) return;
    for (esp_zb_zcl_read_attr_resp_variable_t *v = msg->variables; v; v = v->next) {
        if (v->status != ESP_ZB_ZCL_STATUS_SUCCESS || !v->attribute.data.value) continue;
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
                strlcpy(s_devices[idx].model, model, sizeof(s_devices[idx].model));
                s_devices[idx].supported = strcmp(model, AQARA_MODEL_ID) == 0;
                if (s_devices[idx].supported) {
                    save_devices();
                    xSemaphoreGive(s_devices_lock);
                    ESP_LOGW(TAG, "QBKG25LM model found at 0x%04x; control remains locked until physical channel mapping is confirmed",
                             msg->info.src_address.u.short_addr);
                    return;
                }
                ESP_LOGW(TAG, "Unsupported device 0x%04x model='%s'", msg->info.src_address.u.short_addr, model);
                save_devices();
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
    }
    return ESP_OK;
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
            s_devices[prior].state[0] = s_devices[prior].state[1] = s_devices[prior].state[2] = ZB_CHANNEL_UNKNOWN;
        }
        xSemaphoreGive(s_devices_lock);
        discover_device(d->device_short_addr, d->ieee_addr);
#else
        ESP_LOGI(TAG, "Stage 2 records join only; endpoint discovery is enabled in Stage 3");
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
    esp_zb_cfg_t cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg = {.max_children = APP_COORDINATOR_MAX_CHILDREN},
    };
    esp_zb_init(&cfg);
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
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
    vTaskDelete(NULL);
}

esp_err_t zigbee_manager_start(void)
{
    s_devices_lock = xSemaphoreCreateMutex();
    if (!s_devices_lock) return ESP_ERR_NO_MEM;
    zb_state_cache_init(&s_state_cache, APP_ZIGBEE_STATE_MAX_AGE_MS, APP_ZIGBEE_READ_TIMEOUT_MS);
    load_devices();
    esp_zb_platform_config_t platform = {
        .radio_config = {.radio_mode = ZB_RADIO_MODE_NATIVE},
        .host_config = {.host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE},
    };
    ESP_RETURN_ON_ERROR(esp_zb_platform_config(&platform), TAG, "Zigbee platform config");
    esp_coex_wifi_i154_enable();
    diagnostics_record_memory_event("zigbee_starting");
    return xTaskCreate(zigbee_task, "zigbee_main", 8192, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
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
    esp_zb_lock_acquire(portMAX_DELAY);
    configure_aqara(short_addr, endpoints);
    esp_zb_lock_release();
    ESP_LOGW(TAG, "Physical mapping confirmed short=0x%04x left=%u center=%u right=%u",
             short_addr, endpoints[0], endpoints[1], endpoints[2]);
    return ESP_OK;
#endif
}

esp_err_t zigbee_manager_set_channel(uint16_t addr, unsigned channel, bool on)
{
    if (!s_network_ready || channel > 2) return ESP_ERR_INVALID_ARG;
    uint8_t ep = 0;
    xSemaphoreTake(s_devices_lock, portMAX_DELAY);
    int idx = find_device_by_short(addr);
    if (idx >= 0 && s_devices[idx].supported && s_devices[idx].mapping_confirmed) {
        ep = s_devices[idx].endpoints[channel];
        if (ep) zb_state_cache_begin_read(&s_state_cache, addr, ep, now_ms());
        s_devices[idx].state[channel] = ZB_CHANNEL_UNKNOWN;
    }
    xSemaphoreGive(s_devices_lock);
    if (!ep) return ESP_ERR_NOT_FOUND;
    ESP_LOGI(TAG, "Control TX short=0x%04x channel=%u endpoint=%u value=%s",
             addr, channel, ep, on ? "ON" : "OFF");
    esp_zb_zcl_on_off_cmd_t cmd = {0};
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = addr;
    cmd.zcl_basic_cmd.dst_endpoint = ep;
    cmd.zcl_basic_cmd.src_endpoint = APP_GATEWAY_ENDPOINT;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.on_off_cmd_id = on ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_on_off_cmd_req(&cmd);
    send_read(addr, ep, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
    esp_zb_lock_release();
    return ESP_OK;
}

size_t zigbee_manager_snapshot(zb_device_snapshot_t *out, size_t capacity)
{
    if (!out || !capacity) return 0;
    size_t n = 0;
    xSemaphoreTake(s_devices_lock, portMAX_DELAY);
    for (size_t i = 0; i < APP_MAX_ZIGBEE_DEVICES && n < capacity; ++i) if (s_devices[i].used) {
        out[n] = s_devices[i];
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
        status->updated_us = cached.updated_ms * 1000;
        if (cached.valid) status->state = cached.value == ZB_CACHE_ON ? ZB_CHANNEL_ON : ZB_CHANNEL_OFF;
    }
    xSemaphoreGive(s_devices_lock);
    return status->known_endpoint ? ESP_OK : ESP_ERR_NOT_FOUND;
}
