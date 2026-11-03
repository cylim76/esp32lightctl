#include "scheduler.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "device_control.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "settings.h"
#include "time_manager.h"

#define EXEC_MAGIC 0x45584536UL
#define EXEC_VERSION 2
#define LOG_MAGIC 0x4c4f4736UL

typedef struct {
    uint32_t signature;
    uint32_t date_key;
    time_t due_epoch;
    time_t next_action_epoch;
    uint32_t command_id;
    uint8_t next_retry_index;
    uint8_t attempt_count;
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

static time_t find_next_schedule(time_t now, const app_settings_t *cfg)
{
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
    state->command_id = 0;
    state->next_action_epoch = 0;
    persist_execution();
    add_log(index, now, reason);
}

static bool schedule_retry(unsigned index, execution_state_t *state,
                           const app_schedule_entry_t *entry, time_t now)
{
    int elapsed_minutes = (int)((now - state->due_epoch) / 60);
    while (state->next_retry_index < entry->retry_count &&
           entry->retry_offsets_minutes[state->next_retry_index] <= elapsed_minutes) {
        state->next_retry_index++;
    }
    if (state->next_retry_index >= entry->retry_count) {
        mark_finished(index, state, now, "retry limit reached; automatic OFF stopped");
        return false;
    }
    state->next_action_epoch = state->due_epoch + entry->retry_offsets_minutes[state->next_retry_index] * 60;
    state->next_retry_index++;
    persist_execution();
    return true;
}

static bool command_succeeded(schedule_target_t target, const device_command_result_t *result)
{
    if (target <= SCHEDULE_TARGET_LIGHT_ALL) return result->status == DEVICE_COMMAND_CONFIRMED;
    return result->status == DEVICE_COMMAND_SENT && result->terminal;
}

static void evaluate_entry(unsigned index, const app_schedule_entry_t *entry, time_t now)
{
    struct tm local;
    time_t due = schedule_due_today(now, entry, &local);
    if (!entry->enabled || !(entry->days & (1U << local.tm_wday))) return;
    uint16_t window_minutes = entry->catchup_window_minutes ? entry->catchup_window_minutes : 1;
    time_t deadline = due + (time_t)window_minutes * 60;
    if (now < due) return;
    execution_state_t *state = &s_exec.entries[index];
    uint32_t date_key = local_date_key(&local);
    uint32_t signature = schedule_signature(entry);
    if (state->signature == signature && state->date_key > date_key) return;
    if (now > deadline) {
        if (state->date_key == date_key && state->signature == signature && !state->finished) {
            mark_finished(index, state, now, "catch-up window expired; no cross-day retry");
        }
        return;
    }
    if (state->date_key != date_key || state->signature != signature) {
        memset(state, 0, sizeof(*state));
        state->date_key = date_key;
        state->signature = signature;
        state->due_epoch = due;
        state->next_action_epoch = now;
        persist_execution();
        add_log(index, now, now == due ? "due: queued for unified device control" :
                                        "startup/recovery catch-up started within window");
    }
    if (state->finished) return;

    if (state->command_id) {
        device_command_result_t result;
        esp_err_t err = device_control_get_result(state->command_id, &result);
        if (err == ESP_OK && !result.terminal) return;
        if (err == ESP_OK && command_succeeded((schedule_target_t)entry->target, &result)) {
            char message[160];
            snprintf(message, sizeof(message), "%s command %lu %s; actual=%s",
                     settings_schedule_target_name(entry->target), (unsigned long)result.id,
                     device_command_status_name(result.status),
                     result.actual_state == DEVICE_ACTUAL_OFF ? "OFF" : "UNKNOWN");
            mark_finished(index, state, now, message);
            return;
        }
        char failure[160];
        snprintf(failure, sizeof(failure), "command %lu failed/unavailable: %s",
                 (unsigned long)state->command_id, err == ESP_OK ? result.detail : esp_err_to_name(err));
        add_log(index, now, failure);
        state->command_id = 0;
        if (!schedule_retry(index, state, entry, now)) return;
    }

    if (now < state->next_action_epoch) return;
    uint32_t command_id = 0;
    esp_err_t err = device_control_submit_target((schedule_target_t)entry->target,
                                                  "energy_schedule", &command_id);
    if (err == ESP_OK) {
        state->command_id = command_id;
        state->attempt_count++;
        persist_execution();
        char message[128];
        snprintf(message, sizeof(message), "%s OFF command %lu PENDING (attempt %u)",
                 settings_schedule_target_name(entry->target), (unsigned long)command_id,
                 state->attempt_count);
        add_log(index, now, message);
    } else {
        char message[128];
        snprintf(message, sizeof(message), "%s OFF submit failed: %s",
                 settings_schedule_target_name(entry->target), esp_err_to_name(err));
        add_log(index, now, message);
        schedule_retry(index, state, entry, now);
    }
}

static void scheduler_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (time_manager_is_valid()) {
            time_t now;
            time(&now);
            app_settings_t cfg;
            settings_copy(&cfg);
            for (size_t i = 0; i < cfg.schedule_count && i < APP_MAX_SCHEDULES; ++i) {
                evaluate_entry(i, &cfg.schedules[i], now);
            }
            time_t next = find_next_schedule(now, &cfg);
            portENTER_CRITICAL(&s_lock);
            s_status.next_schedule_epoch = next;
            portEXIT_CRITICAL(&s_lock);
        } else {
            portENTER_CRITICAL(&s_lock);
            s_status.next_schedule_epoch = 0;
            strlcpy(s_status.last_result, "skipped: system time INVALID", sizeof(s_status.last_result));
            portEXIT_CRITICAL(&s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
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
