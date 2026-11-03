#include "scheduler.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_config.h"
#include "device_control.h"
#include "diagnostics.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "scheduler_persistence.h"
#include "settings.h"
#include "storage_manager.h"
#include "time_manager.h"

#define LEGACY_EXEC_MAGIC 0x45584536UL
#define LEGACY_EXEC_VERSION 2
#define LOG_MAGIC 0x4c4f4736UL

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t reserved[3];
    scheduler_execution_state_t entries[APP_MAX_SCHEDULES];
} legacy_execution_store_t;

typedef struct {
    uint32_t magic;
    uint8_t count;
    uint8_t reserved[3];
    scheduler_log_entry_t entries[SCHEDULER_LOG_CAPACITY];
} log_store_t;

static const char *TAG = "scheduler";
static scheduler_execution_state_t s_exec[APP_MAX_SCHEDULES];
static scheduler_store_t s_store;
static nvs_handle_t s_runtime_nvs;
static bool s_storage_ready;
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

static void entry_key(unsigned index, char key[8])
{
    snprintf(key, 8, "ex%02u", index);
}

static esp_err_t runtime_read(void *ctx, unsigned index, void *data, size_t *size)
{
    nvs_handle_t *handle = ctx;
    char key[8];
    entry_key(index, key);
    return nvs_get_blob(*handle, key, data, size);
}

static esp_err_t runtime_write(void *ctx, unsigned index, const void *data, size_t size)
{
    nvs_handle_t *handle = ctx;
    char key[8];
    entry_key(index, key);
    esp_err_t err = nvs_set_blob(*handle, key, data, size);
    if (err == ESP_OK) err = nvs_commit(*handle);
    storage_manager_record_write(APP_RUNTIME_STORAGE_PARTITION, key, err, true);
    return err;
}

static esp_err_t persist_execution(unsigned index)
{
    bool changed = false;
    esp_err_t err = scheduler_store_save(&s_store, index, &s_exec[index], &changed);
    if (!changed && err == ESP_OK) {
        storage_manager_record_write(APP_RUNTIME_STORAGE_PARTITION, "execution", ESP_OK, false);
    }
    if (err != ESP_OK) {
        s_storage_ready = false;
        portENTER_CRITICAL(&s_lock);
        s_status.next_schedule_epoch = 0;
        strlcpy(s_status.last_result, "automation stopped: runtime NVS write failed",
                sizeof(s_status.last_result));
        portEXIT_CRITICAL(&s_lock);
        diagnostics_set_error(TAG, "execution state %u persist failed: %s; automation stopped",
                              index, esp_err_to_name(err));
        ESP_LOGE(TAG, "Execution state %u persist failed: %s; automation stopped",
                 index, esp_err_to_name(err));
    }
    return err;
}

static void add_log(unsigned index, time_t now, const char *message)
{
    portENTER_CRITICAL(&s_lock);
    for (int i = SCHEDULER_LOG_CAPACITY - 1; i > 0; --i) s_status.logs[i] = s_status.logs[i - 1];
    s_status.logs[0].epoch = now;
    s_status.logs[0].schedule_index = index;
    strlcpy(s_status.logs[0].message, message, sizeof(s_status.logs[0].message));
    if (s_status.log_count < SCHEDULER_LOG_CAPACITY) s_status.log_count++;
    s_status.last_action_epoch = now;
    strlcpy(s_status.last_result, message, sizeof(s_status.last_result));
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Schedule %u: %s", index + 1, message);
}

static void load_state(void)
{
    memset(&s_exec, 0, sizeof(s_exec));
    strlcpy(s_status.last_result, "No automatic close action yet", sizeof(s_status.last_result));
    s_storage_ready = false;

    if (!storage_manager_runtime_ready()) {
        strlcpy(s_status.last_result, "automation disabled: runtime NVS unavailable",
                sizeof(s_status.last_result));
        return;
    }
    esp_err_t err = nvs_open_from_partition(APP_RUNTIME_STORAGE_PARTITION, "energy",
                                             NVS_READWRITE, &s_runtime_nvs);
    if (err != ESP_OK) {
        storage_manager_record_write(APP_RUNTIME_STORAGE_PARTITION, "open", err, true);
        strlcpy(s_status.last_result, "automation disabled: runtime NVS open failed",
                sizeof(s_status.last_result));
        return;
    }
    scheduler_store_backend_t backend = {
        .ctx = &s_runtime_nvs,
        .read = runtime_read,
        .write = runtime_write,
    };
    scheduler_store_init(&s_store, &backend);
    s_storage_ready = true;
    bool runtime_present[APP_MAX_SCHEDULES] = {0};
    for (unsigned i = 0; i < APP_MAX_SCHEDULES; ++i) {
        err = scheduler_store_load(&s_store, i, &s_exec[i]);
        if (err == ESP_OK) runtime_present[i] = true;
        else if (err != ESP_ERR_NVS_NOT_FOUND) {
            storage_manager_record_corruption(APP_RUNTIME_STORAGE_PARTITION,
                                              "execution record CRC/version invalid");
            s_storage_ready = false;
        }
    }

    /* Non-destructive migration is restartable: preserve valid per-entry records
       and fill only missing entries from the old whole blob. Never erase it. */
    if (s_storage_ready && storage_manager_primary_ready()) {
        nvs_handle_t legacy_nvs;
        if (nvs_open("energy", NVS_READONLY, &legacy_nvs) == ESP_OK) {
            legacy_execution_store_t legacy = {0};
            size_t size = sizeof(legacy);
            err = nvs_get_blob(legacy_nvs, "exec_v1", &legacy, &size);
            if (err == ESP_OK && size == sizeof(legacy) && legacy.magic == LEGACY_EXEC_MAGIC &&
                legacy.version == LEGACY_EXEC_VERSION) {
                unsigned migrated = 0;
                for (unsigned i = 0; i < APP_MAX_SCHEDULES; ++i) {
                    if (runtime_present[i]) continue;
                    s_exec[i] = legacy.entries[i];
                    if ((s_exec[i].signature || s_exec[i].date_key) && persist_execution(i) != ESP_OK) break;
                    if (s_exec[i].signature || s_exec[i].date_key) migrated++;
                }
                if (migrated) {
                    ESP_LOGI(TAG, "Migrated %u missing legacy exec_v1 entries; legacy blob retained",
                             migrated);
                }
            } else if (err != ESP_ERR_NVS_NOT_FOUND) {
                storage_manager_record_corruption("nvs", "legacy exec_v1 invalid; retained");
            }
            nvs_close(legacy_nvs);
        }
    }

    /* Legacy logs are loaded once for operator context but are no longer written to Flash. */
    nvs_handle_t nvs;
    if (storage_manager_primary_ready() && nvs_open("energy", NVS_READONLY, &nvs) == ESP_OK) {
        log_store_t logs;
        size_t size = sizeof(logs);
        if (nvs_get_blob(nvs, "log_v1", &logs, &size) == ESP_OK && size == sizeof(logs) &&
            logs.magic == LOG_MAGIC) {
            s_status.log_count = logs.count <= SCHEDULER_LOG_CAPACITY ? logs.count : SCHEDULER_LOG_CAPACITY;
            memcpy(s_status.logs, logs.entries, sizeof(s_status.logs));
            if (s_status.log_count) {
                s_status.last_action_epoch = s_status.logs[0].epoch;
                strlcpy(s_status.last_result, s_status.logs[0].message, sizeof(s_status.last_result));
            }
        }
        nvs_close(nvs);
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

static void mark_finished(unsigned index, scheduler_execution_state_t *state, time_t now, const char *reason)
{
    state->finished = true;
    state->command_id = 0;
    state->next_action_epoch = 0;
    if (persist_execution(index) == ESP_OK) add_log(index, now, reason);
    else add_log(index, now, "result reached but persistence failed; automation stopped");
}

static bool schedule_retry(unsigned index, scheduler_execution_state_t *state,
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
    if (persist_execution(index) != ESP_OK) {
        add_log(index, now, "retry state persistence failed; automation stopped");
        return false;
    }
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
    scheduler_execution_state_t *state = &s_exec[index];
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
        if (persist_execution(index) != ESP_OK) {
            add_log(index, now, "new execution state could not be persisted; OFF not submitted");
            return;
        }
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
    /* Persist the attempt before sending. A reset after the OFF command can only
       consume a bounded retry, never restart an unbounded attempt sequence. */
    state->attempt_count++;
    state->command_id = UINT32_MAX;
    if (persist_execution(index) != ESP_OK) {
        add_log(index, now, "attempt marker persistence failed; OFF not submitted");
        return;
    }
    uint32_t command_id = 0;
    esp_err_t err = device_control_submit_target((schedule_target_t)entry->target,
                                                  "energy_schedule", &command_id);
    if (err == ESP_OK) {
        /* The durable pre-submit marker above is sufficient for reset safety.
           Keep the transient command ID in RAM to avoid a second Flash commit. */
        state->command_id = command_id;
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
        state->command_id = 0;
        schedule_retry(index, state, entry, now);
    }
}

static void scheduler_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_storage_ready) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
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
    if (!s_storage_ready) {
        portENTER_CRITICAL(&s_lock);
        s_status.next_schedule_epoch = 0;
        strlcpy(s_status.last_result, "automation disabled: persistent runtime state unavailable",
                sizeof(s_status.last_result));
        portEXIT_CRITICAL(&s_lock);
    }
    return xTaskCreate(scheduler_task, "scheduler", 6144, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void scheduler_get_status(scheduler_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}
