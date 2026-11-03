#include "scheduler.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "settings.h"
#include "stage_config.h"
#include "time_manager.h"
#include "zigbee_manager.h"
#if LIGHTGW_HAS_IR
#include "ir_manager.h"
#endif

#define EXEC_MAGIC 0x45584536UL
#define EXEC_VERSION 1
#define LOG_MAGIC 0x4c4f4736UL

typedef struct {
    uint32_t signature;
    uint32_t date_key;
    time_t due_epoch;
    time_t next_action_epoch;
    uint8_t pending_light_mask;
    uint8_t pending_ac_mask;
    uint8_t next_retry_index;
    uint8_t final_verification;
    uint8_t finished;
} execution_state_t;

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t reserved[3];
    execution_state_t entries[APP_MAX_SCHEDULES];
} execution_store_t;

typedef struct {
    uint32_t magic;
    uint8_t count;
    uint8_t reserved[3];
    scheduler_log_entry_t entries[SCHEDULER_LOG_CAPACITY];
} log_store_t;

static const char *TAG = "scheduler";
static execution_store_t s_exec;
static scheduler_status_t s_status;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static uint8_t target_light_mask(uint8_t target)
{
    if (target <= SCHEDULE_TARGET_LIGHT_RIGHT) return 1U << target;
    return target == SCHEDULE_TARGET_LIGHT_ALL ? 0x07 : 0;
}

static uint8_t target_ac_mask(uint8_t target)
{
    if (target == SCHEDULE_TARGET_AC_1) return 0x01;
    if (target == SCHEDULE_TARGET_AC_2) return 0x02;
    if (target == SCHEDULE_TARGET_AC_GROUP) return 0x03;
    return 0;
}

static uint32_t schedule_signature(const app_schedule_entry_t *entry)
{
    const uint8_t *bytes = (const uint8_t *)entry;
    uint32_t hash = 2166136261UL;
    for (size_t i = 0; i < sizeof(*entry); ++i) hash = (hash ^ bytes[i]) * 16777619UL;
    return hash;
}

static uint32_t local_date_key(const struct tm *local)
{
    return (uint32_t)(local->tm_year + 1900) * 10000U + (uint32_t)(local->tm_mon + 1) * 100U + local->tm_mday;
}

static void save_blob(const char *key, const void *data, size_t size)
{
    nvs_handle_t nvs;
    if (nvs_open("energy", NVS_READWRITE, &nvs) != ESP_OK) return;
    esp_err_t err = nvs_set_blob(nvs, key, data, size);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) ESP_LOGE(TAG, "Persist %s failed: %s", key, esp_err_to_name(err));
}

static void persist_execution(void) { save_blob("exec_v1", &s_exec, sizeof(s_exec)); }

static void add_log(unsigned index, time_t now, const char *message)
{
    log_store_t store = {.magic = LOG_MAGIC};
    portENTER_CRITICAL(&s_lock);
    for (int i = SCHEDULER_LOG_CAPACITY - 1; i > 0; --i) s_status.logs[i] = s_status.logs[i - 1];
    s_status.logs[0].epoch = now;
    s_status.logs[0].schedule_index = index;
    strlcpy(s_status.logs[0].message, message, sizeof(s_status.logs[0].message));
    if (s_status.log_count < SCHEDULER_LOG_CAPACITY) s_status.log_count++;
    s_status.last_action_epoch = now;
    strlcpy(s_status.last_result, message, sizeof(s_status.last_result));
    store.count = s_status.log_count;
    memcpy(store.entries, s_status.logs, sizeof(store.entries));
    portEXIT_CRITICAL(&s_lock);
    save_blob("log_v1", &store, sizeof(store));
    ESP_LOGI(TAG, "Schedule %u: %s", index + 1, message);
}

static void load_state(void)
{
    memset(&s_exec, 0, sizeof(s_exec));
    s_exec.magic = EXEC_MAGIC;
    s_exec.version = EXEC_VERSION;
    strlcpy(s_status.last_result, "No automatic close action yet", sizeof(s_status.last_result));
    nvs_handle_t nvs;
    if (nvs_open("energy", NVS_READONLY, &nvs) != ESP_OK) return;
    size_t size = sizeof(s_exec);
    if (nvs_get_blob(nvs, "exec_v1", &s_exec, &size) != ESP_OK || size != sizeof(s_exec) ||
        s_exec.magic != EXEC_MAGIC || s_exec.version != EXEC_VERSION) {
        memset(&s_exec, 0, sizeof(s_exec));
        s_exec.magic = EXEC_MAGIC;
        s_exec.version = EXEC_VERSION;
    }
    log_store_t logs;
    size = sizeof(logs);
    if (nvs_get_blob(nvs, "log_v1", &logs, &size) == ESP_OK && size == sizeof(logs) && logs.magic == LOG_MAGIC) {
        s_status.log_count = logs.count <= SCHEDULER_LOG_CAPACITY ? logs.count : SCHEDULER_LOG_CAPACITY;
        memcpy(s_status.logs, logs.entries, sizeof(s_status.logs));
        if (s_status.log_count) {
            s_status.last_action_epoch = s_status.logs[0].epoch;
            strlcpy(s_status.last_result, s_status.logs[0].message, sizeof(s_status.last_result));
        }
    }
    nvs_close(nvs);
}

static uint8_t confirmed_light_mask(uint8_t selected)
{
    zb_device_snapshot_t devices[APP_MAX_ZIGBEE_DEVICES];
    size_t count = zigbee_manager_snapshot(devices, APP_MAX_ZIGBEE_DEVICES);
    uint8_t confirmed = 0;
    for (unsigned channel = 0; channel < 3; ++channel) {
        if (!(selected & (1U << channel))) continue;
        bool any = false, all_off = true;
        for (size_t i = 0; i < count; ++i) {
            if (!devices[i].supported || !devices[i].mapping_confirmed) continue;
            any = true;
            if (devices[i].state[channel] != ZB_CHANNEL_OFF) all_off = false;
        }
        if (any && all_off) confirmed |= 1U << channel;
    }
    return confirmed;
}

static void send_lights(uint8_t mask, unsigned *submitted, unsigned *failed)
{
    zb_device_snapshot_t devices[APP_MAX_ZIGBEE_DEVICES];
    size_t count = zigbee_manager_snapshot(devices, APP_MAX_ZIGBEE_DEVICES);
    for (size_t i = 0; i < count; ++i) {
        if (!devices[i].supported || !devices[i].mapping_confirmed) continue;
        for (unsigned channel = 0; channel < 3; ++channel) {
            if (!(mask & (1U << channel)) || devices[i].state[channel] == ZB_CHANNEL_OFF) continue;
            esp_err_t err = zigbee_manager_set_channel(devices[i].short_addr, channel, false);
            if (err == ESP_OK) (*submitted)++; else (*failed)++;
        }
    }
}

static time_t schedule_due_today(time_t now, const app_schedule_entry_t *entry, struct tm *local_out)
{
    struct tm local;
    localtime_r(&now, &local);
    if (local_out) *local_out = local;
    local.tm_hour = entry->hour;
    local.tm_min = entry->minute;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    return mktime(&local);
}

static time_t find_next_schedule(time_t now)
{
    const app_settings_t *cfg = settings_get();
    time_t best = 0;
    for (size_t i = 0; i < cfg->schedule_count; ++i) {
        const app_schedule_entry_t *entry = &cfg->schedules[i];
        if (!entry->enabled) continue;
        for (int day_offset = 0; day_offset < 8; ++day_offset) {
            struct tm local;
            localtime_r(&now, &local);
            local.tm_mday += day_offset;
            local.tm_hour = entry->hour;
            local.tm_min = entry->minute;
            local.tm_sec = 0;
            local.tm_isdst = -1;
            time_t candidate = mktime(&local);
            localtime_r(&candidate, &local);
            if ((entry->days & (1U << local.tm_wday)) && candidate > now && (!best || candidate < best)) best = candidate;
        }
    }
    return best;
}

static void mark_finished(unsigned index, execution_state_t *state, time_t now, const char *reason)
{
    state->finished = true;
    state->next_action_epoch = 0;
    persist_execution();
    add_log(index, now, reason);
}

#if LIGHTGW_HAS_IR
static uint8_t usable_ac_mask(uint8_t requested)
{
    ir_status_t status;
    ir_manager_get_status(&status);
    uint8_t available = (status.code_saved[0] ? 1 : 0) | (status.code_saved[1] ? 2 : 0);
    if (requested == 3 && available == 1) return 1; /* Shared broad-beam OFF code in slot 1. */
    return requested & available;
}
#endif

static void evaluate_entry(unsigned index, const app_schedule_entry_t *entry, time_t now)
{
    struct tm local;
    time_t due = schedule_due_today(now, entry, &local);
    if (!entry->enabled || !(entry->days & (1U << local.tm_wday))) return;
    /* Zero catch-up still permits execution during the scheduled minute. */
    uint16_t window_minutes = entry->catchup_window_minutes ? entry->catchup_window_minutes : 1;
    time_t deadline = due + (time_t)window_minutes * 60;
    if (now < due) return;
    execution_state_t *state = &s_exec.entries[index];
    uint32_t date_key = local_date_key(&local);
    uint32_t signature = schedule_signature(entry);
    if (state->signature == signature && state->date_key > date_key) {
        return; /* Time moved backward: fail safe instead of repeating an already visited date. */
    }
    if (now > deadline) {
        if (state->date_key == date_key && state->signature == signature && !state->finished) {
            mark_finished(index, state, now, "window expired: bounded retries stopped; no cross-day retry");
        }
        return;
    }
    if (state->date_key != date_key || state->signature != signature) {
        memset(state, 0, sizeof(*state));
        state->date_key = date_key;
        state->signature = signature;
        state->due_epoch = due;
        state->next_action_epoch = now;
        state->pending_light_mask = target_light_mask(entry->target);
        state->pending_ac_mask = target_ac_mask(entry->target);
        persist_execution();
        add_log(index, now, now == due ? "due: execution started" : "startup/recovery catch-up started within window");
    }
    if (state->finished) return;
    state->pending_light_mask &= ~confirmed_light_mask(state->pending_light_mask);
    if (!state->pending_light_mask && !state->pending_ac_mask) {
        mark_finished(index, state, now, "completed: light OFF confirmed; IR sent if selected (AC state unconfirmed)");
        return;
    }
    if (now >= deadline) {
        char result[128];
        snprintf(result, sizeof(result), "window expired: pending lights=0x%02x AC=0x%02x; no cross-day retry",
                 state->pending_light_mask, state->pending_ac_mask);
        mark_finished(index, state, now, result);
        return;
    }
    if (state->final_verification) {
        if (now >= state->next_action_epoch) {
            state->pending_light_mask &= ~confirmed_light_mask(state->pending_light_mask);
            char result[128];
            snprintf(result, sizeof(result), "retry limit reached: pending lights=0x%02x AC=0x%02x",
                     state->pending_light_mask, state->pending_ac_mask);
            mark_finished(index, state, now, result);
        }
        return;
    }
    if (now < state->next_action_epoch) return;

    unsigned submitted = 0, failed = 0;
    if (state->pending_light_mask && zigbee_manager_network_ready()) send_lights(state->pending_light_mask, &submitted, &failed);
#if LIGHTGW_HAS_IR
    if (state->pending_ac_mask) {
        uint8_t mask = usable_ac_mask(state->pending_ac_mask);
        esp_err_t err = mask ? ir_manager_enqueue_send(mask, 1, "energy_schedule") : ESP_ERR_NOT_FOUND;
        if (err == ESP_OK) { state->pending_ac_mask = 0; submitted++; } else failed++;
    }
#endif
    char result[128];
    snprintf(result, sizeof(result), "%s OFF attempt: submitted=%u failed=%u pending_lights=0x%02x pending_AC=0x%02x",
             settings_schedule_target_name(entry->target), submitted, failed,
             state->pending_light_mask, state->pending_ac_mask);
    add_log(index, now, result);
    int elapsed_minutes = (int)((now - due) / 60);
    while (state->next_retry_index < entry->retry_count &&
           entry->retry_offsets_minutes[state->next_retry_index] <= elapsed_minutes) state->next_retry_index++;
    if (state->next_retry_index < entry->retry_count) {
        state->next_action_epoch = due + entry->retry_offsets_minutes[state->next_retry_index] * 60;
        state->next_retry_index++;
    } else {
        state->final_verification = true;
        state->next_action_epoch = now + 60;
        if (state->next_action_epoch > deadline) state->next_action_epoch = deadline;
    }
    persist_execution();
}

static void scheduler_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (time_manager_is_valid()) {
            time_t now;
            time(&now);
            const app_settings_t *cfg = settings_get();
            for (size_t i = 0; i < cfg->schedule_count && i < APP_MAX_SCHEDULES; ++i) evaluate_entry(i, &cfg->schedules[i], now);
            time_t next = find_next_schedule(now);
            portENTER_CRITICAL(&s_lock);
            s_status.next_schedule_epoch = next;
            portEXIT_CRITICAL(&s_lock);
        } else {
            portENTER_CRITICAL(&s_lock);
            s_status.next_schedule_epoch = 0;
            strlcpy(s_status.last_result, "skipped: system time INVALID", sizeof(s_status.last_result));
            portEXIT_CRITICAL(&s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

esp_err_t scheduler_start(void)
{
    memset(&s_status, 0, sizeof(s_status));
    load_state();
    return xTaskCreate(scheduler_task, "scheduler", 6144, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void scheduler_get_status(scheduler_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}
