#include "ir_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "app_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ir_receiver.h"
#include "ir_storage.h"
#include "ir_transmitter.h"
#include "settings.h"
#include "time_manager.h"

#define IR_TX_QUEUE_DEPTH 4
#define IR_LEARN_MAX_ATTEMPTS 8
#define IR_CAPTURE_TIMEOUT_MS 20000

typedef struct {
    uint32_t id;
    uint8_t slot_mask;
    uint8_t repeats;
    char origin[24];
} ir_tx_job_t;

static const char *TAG = "ir_manager";
static QueueHandle_t s_tx_queue;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static ir_status_t s_status;
static ir_code_t s_candidate;
static ir_task_result_t s_task_history[APP_IR_TASK_HISTORY];
static uint32_t s_next_task_id;

static ir_task_result_t *task_record(uint32_t id)
{
    ir_task_result_t *record = &s_task_history[id % APP_IR_TASK_HISTORY];
    return record->id == id ? record : NULL;
}

static void update_task(uint32_t id, ir_task_state_t state, const char *result, bool completed)
{
    portENTER_CRITICAL(&s_lock);
    ir_task_result_t *record = task_record(id);
    if (record) {
        record->state = state;
        if (result) strlcpy(record->result, result, sizeof(record->result));
        if (completed) record->completed_us = esp_timer_get_time();
    }
    portEXIT_CRITICAL(&s_lock);
}

static bool codes_similar(const ir_code_t *a, const ir_code_t *b)
{
    if (a->duration_count != b->duration_count) return false;
    for (size_t i = 0; i < a->duration_count; ++i) {
        uint32_t expected = a->durations_us[i];
        uint32_t margin = expected / 4;
        if (margin < 200) margin = 200;
        uint32_t actual = b->durations_us[i];
        if (actual + margin < expected || actual > expected + margin) return false;
    }
    return true;
}

static void average_code(ir_code_t *average, const ir_code_t *sample, unsigned previous_samples)
{
    average->total_duration_us = 0;
    for (size_t i = 0; i < average->duration_count; ++i) {
        average->durations_us[i] =
            (average->durations_us[i] * previous_samples + sample->durations_us[i]) / (previous_samples + 1);
        average->total_duration_us += average->durations_us[i];
    }
    if (sample->protocol > average->protocol) {
        average->protocol = sample->protocol;
        average->decoded_value = sample->decoded_value;
    }
}

static void set_last_result(const char *message, uint8_t slot_mask)
{
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_status.last_send_result, message, sizeof(s_status.last_send_result));
    s_status.last_send_slot_mask = slot_mask;
    s_status.last_send_uptime_seconds = esp_timer_get_time() / 1000000;
    if (time_manager_is_valid()) {
        time_t now;
        time(&now);
        s_status.last_send_epoch = now;
    } else {
        s_status.last_send_epoch = 0;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void learning_task(void *arg)
{
    uint8_t slot = (uintptr_t)arg;
    esp_err_t err = ir_receiver_open();
    ir_code_t *average = calloc(1, sizeof(*average));
    ir_code_t *sample = malloc(sizeof(*sample));
    if (!average || !sample) err = ESP_ERR_NO_MEM;
    unsigned stable = 0;
    unsigned attempts = 0;
    while (err == ESP_OK && stable < IR_CAPTURE_REQUIRED && attempts < IR_LEARN_MAX_ATTEMPTS) {
        attempts++;
        ESP_LOGI(TAG, "Learning slot %u: press the explicit LG OFF key (capture %u/%u, attempt %u/%u)",
                 slot + 1, stable + 1, IR_CAPTURE_REQUIRED, attempts, IR_LEARN_MAX_ATTEMPTS);
        err = ir_receiver_capture(sample, IR_CAPTURE_TIMEOUT_MS);
        if (err != ESP_OK) break;
        if (stable == 0 || !codes_similar(average, sample)) {
            if (stable) ESP_LOGW(TAG, "Capture differs from prior samples; stability count restarted");
            *average = *sample;
            stable = 1;
        } else {
            average_code(average, sample, stable);
            stable++;
        }
        portENTER_CRITICAL(&s_lock);
        s_status.stable_captures = stable;
        s_status.attempts = attempts;
        portEXIT_CRITICAL(&s_lock);
    }
    ir_receiver_close();
    portENTER_CRITICAL(&s_lock);
    s_status.learning = false;
    s_status.attempts = attempts;
    s_status.stable_captures = stable;
    if (err == ESP_OK && stable >= IR_CAPTURE_REQUIRED) {
        average->crc32 = ir_code_crc(average);
        s_candidate = *average;
        s_status.candidate_ready = true;
        s_status.candidate_duration_count = average->duration_count;
        s_status.candidate_protocol = average->protocol;
        s_status.candidate_total_us = average->total_duration_us;
    } else {
        s_status.candidate_ready = false;
        snprintf(s_status.last_send_result, sizeof(s_status.last_send_result),
                 "IR learning failed: %s; no code saved", esp_err_to_name(err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err));
    }
    portEXIT_CRITICAL(&s_lock);
    if (err == ESP_OK && stable >= IR_CAPTURE_REQUIRED) {
        ESP_LOGI(TAG, "Learning candidate ready for confirmation: slot=%u stable=%u durations=%u protocol=%s",
                 slot + 1, stable, average->duration_count, ir_protocol_name(average->protocol));
    } else {
        ESP_LOGE(TAG, "Learning failed: slot=%u stable=%u attempts=%u reason=%s", slot + 1, stable, attempts,
                 esp_err_to_name(err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err));
    }
    free(sample);
    free(average);
    vTaskDelete(NULL);
}

static void refresh_code_metadata(uint8_t slot)
{
    ir_code_t *code = malloc(sizeof(*code));
    esp_err_t err = code ? ir_storage_load_code(slot, code) : ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_lock);
    s_status.code_saved[slot] = err == ESP_OK;
    if (err == ESP_OK) {
        s_status.duration_count[slot] = code->duration_count;
        s_status.carrier_hz[slot] = code->carrier_hz;
        s_status.duty_percent[slot] = code->duty_percent;
        s_status.protocol[slot] = code->protocol;
        s_status.code_crc[slot] = code->crc32;
    } else {
        s_status.duration_count[slot] = 0;
        s_status.carrier_hz[slot] = 0;
        s_status.duty_percent[slot] = 0;
        s_status.protocol[slot] = IR_PROTOCOL_RAW;
        s_status.code_crc[slot] = 0;
    }
    portEXIT_CRITICAL(&s_lock);
    free(code);
}

static void migrate_legacy_schedule(void)
{
    ir_schedule_t schedule = s_status.schedule;
    if (!schedule.enabled) return;
    app_settings_t settings_snapshot;
    settings_copy(&settings_snapshot);
    const app_settings_t *settings = &settings_snapshot;
    for (size_t i = 0; i < settings->schedule_count; ++i) {
        if (settings->schedules[i].target >= SCHEDULE_TARGET_AC_1) {
            schedule.enabled = false;
            ir_storage_save_schedule(&schedule);
            s_status.schedule = schedule;
            strlcpy(s_status.schedule_skip_reason, "legacy IR schedule disabled; per-device AC schedules exist",
                    sizeof(s_status.schedule_skip_reason));
            return;
        }
    }
    app_schedule_entry_t migrated[APP_MAX_SCHEDULES];
    memcpy(migrated, settings->schedules, settings->schedule_count * sizeof(*migrated));
    size_t count = settings->schedule_count;
    uint8_t target = schedule.slot_mask == 1 ? SCHEDULE_TARGET_AC_1 :
                     schedule.slot_mask == 2 ? SCHEDULE_TARGET_AC_2 : SCHEDULE_TARGET_AC_GROUP;
    size_t added = 0;
    if (schedule.use_light_schedules) {
        for (size_t i = 0; i < settings->schedule_count; ++i) {
            const app_schedule_entry_t *entry = &settings->schedules[i];
            if (entry->target <= SCHEDULE_TARGET_LIGHT_ALL && count < APP_MAX_SCHEDULES &&
                added < APP_MAX_SCHEDULES_PER_TARGET) {
                migrated[count] = *entry;
                migrated[count++].target = target;
                added++;
            }
        }
    } else if (count < APP_MAX_SCHEDULES) {
        migrated[count++] = (app_schedule_entry_t){
            .enabled = true, .target = target, .hour = schedule.hour, .minute = schedule.minute,
            .days = schedule.days, .retry_count = 3, .retry_offsets_minutes = {1, 5, 10},
            .catchup_window_minutes = 30,
        };
        added = 1;
    }
    esp_err_t err = added ? settings_save_schedules(migrated, count, settings->timezone) : ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) {
        schedule.enabled = false;
        ir_storage_save_schedule(&schedule);
        s_status.schedule = schedule;
        snprintf(s_status.schedule_skip_reason, sizeof(s_status.schedule_skip_reason),
                 "migrated %u legacy IR schedule(s); legacy executor disabled", (unsigned)added);
        ESP_LOGI(TAG, "%s", s_status.schedule_skip_reason);
    } else {
        snprintf(s_status.schedule_skip_reason, sizeof(s_status.schedule_skip_reason),
                 "legacy IR schedule not executed; migration failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_status.schedule_skip_reason);
    }
}

static void tx_task(void *arg)
{
    (void)arg;
    for (;;) {
        ir_tx_job_t job;
        if (xQueueReceive(s_tx_queue, &job, pdMS_TO_TICKS(1000)) == pdTRUE) {
            update_task(job.id, IR_TASK_TRANSMITTING, "RMT transmission in progress", false);
            portENTER_CRITICAL(&s_lock);
            s_status.transmitting = true;
            portEXIT_CRITICAL(&s_lock);
            esp_err_t result = ESP_OK;
            unsigned sent = 0;
            for (uint8_t slot = 0; slot < IR_CODE_SLOT_COUNT; ++slot) {
                if (!(job.slot_mask & (1U << slot))) continue;
                ir_code_t *code = malloc(sizeof(*code));
                if (!code) { result = ESP_ERR_NO_MEM; break; }
                result = ir_storage_load_code(slot, code);
                if (result == ESP_OK) result = ir_transmitter_send(code, job.repeats, 100);
                free(code);
                if (result != ESP_OK) break;
                sent++;
                if (slot == 0 && (job.slot_mask & 0x02)) vTaskDelay(pdMS_TO_TICKS(250));
            }
            char message[128];
            if (result == ESP_OK && sent) {
                snprintf(message, sizeof(message), "IR sent (%s, %u code%s); AC actual state is unconfirmed",
                         job.origin, sent, sent == 1 ? "" : "s");
                ESP_LOGI(TAG, "%s", message);
            } else {
                snprintf(message, sizeof(message), "IR send failed (%s): %s; AC state unconfirmed",
                         job.origin, esp_err_to_name(result == ESP_OK ? ESP_ERR_NOT_FOUND : result));
                ESP_LOGE(TAG, "%s", message);
            }
            set_last_result(message, job.slot_mask);
            update_task(job.id, result == ESP_OK && sent ? IR_TASK_SENT : IR_TASK_FAILED,
                        message, true);
            portENTER_CRITICAL(&s_lock);
            s_status.transmitting = false;
            portEXIT_CRITICAL(&s_lock);
        }
    }
}

esp_err_t ir_manager_start(void)
{
    memset(&s_status, 0, sizeof(s_status));
    strlcpy(s_status.last_send_result, "No IR transmission yet", sizeof(s_status.last_send_result));
    strlcpy(s_status.schedule_skip_reason, "disabled", sizeof(s_status.schedule_skip_reason));
    ESP_RETURN_ON_ERROR(ir_storage_init(), TAG, "storage init");
    ESP_RETURN_ON_ERROR(ir_transmitter_init(), TAG, "transmitter init");
    ir_storage_load_schedule(&s_status.schedule);
    migrate_legacy_schedule();
    s_tx_queue = xQueueCreate(IR_TX_QUEUE_DEPTH, sizeof(ir_tx_job_t));
    if (!s_tx_queue) return ESP_ERR_NO_MEM;
    for (uint8_t slot = 0; slot < IR_CODE_SLOT_COUNT; ++slot) refresh_code_metadata(slot);
    s_status.initialized = true;
    if (xTaskCreate(tx_task, "ir_tx_manager", 6144, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Stage 5 IR manager started; TX GPIO%d RX GPIO%d; hardware validation NOT TESTED",
             APP_IR_TX_GPIO, APP_IR_RX_GPIO);
    return ESP_OK;
}

esp_err_t ir_manager_start_learning(uint8_t slot)
{
    if (slot >= IR_CODE_SLOT_COUNT || !s_status.initialized) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    if (s_status.learning || s_status.transmitting || (s_tx_queue && uxQueueMessagesWaiting(s_tx_queue))) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_status.learning = true;
    s_status.learning_slot = slot;
    s_status.stable_captures = 0;
    s_status.attempts = 0;
    s_status.candidate_ready = false;
    portEXIT_CRITICAL(&s_lock);
    if (xTaskCreate(learning_task, "ir_learning", 6144, (void *)(uintptr_t)slot, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_lock);
        s_status.learning = false;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ir_manager_save_candidate(uint8_t slot, uint32_t carrier_hz, uint16_t duty_percent)
{
    if (slot >= IR_CODE_SLOT_COUNT || carrier_hz < 30000 || carrier_hz > 60000 ||
        duty_percent < 10 || duty_percent > 50) return ESP_ERR_INVALID_ARG;
    ir_code_t candidate;
    portENTER_CRITICAL(&s_lock);
    bool ready = s_status.candidate_ready && s_status.learning_slot == slot && !s_status.learning;
    candidate = s_candidate;
    portEXIT_CRITICAL(&s_lock);
    if (!ready) return ESP_ERR_INVALID_STATE;
    candidate.carrier_hz = carrier_hz;
    candidate.duty_percent = duty_percent;
    candidate.carrier_measured = false;
    candidate.crc32 = ir_code_crc(&candidate);
    esp_err_t err = ir_storage_save_code(slot, &candidate);
    if (err == ESP_OK) {
        refresh_code_metadata(slot);
        portENTER_CRITICAL(&s_lock);
        s_status.candidate_ready = false;
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}

esp_err_t ir_manager_submit_send(uint8_t slot_mask, uint8_t repeats, const char *origin,
                                 uint32_t *task_id)
{
    if (!task_id) return ESP_ERR_INVALID_ARG;
    if (!s_tx_queue || !(slot_mask & 0x03U) || (slot_mask & ~0x03U) || repeats < 1 || repeats > 2) {
        return ESP_ERR_INVALID_ARG;
    }
    for (uint8_t slot = 0; slot < IR_CODE_SLOT_COUNT; ++slot) {
        if ((slot_mask & (1U << slot)) && !ir_storage_code_exists(slot)) return ESP_ERR_NOT_FOUND;
    }
    portENTER_CRITICAL(&s_lock);
    bool learning = s_status.learning;
    portEXIT_CRITICAL(&s_lock);
    if (learning) return ESP_ERR_INVALID_STATE;
    ir_tx_job_t job = {.slot_mask = slot_mask, .repeats = repeats};
    portENTER_CRITICAL(&s_lock);
    job.id = ++s_next_task_id;
    if (!job.id) job.id = ++s_next_task_id;
    ir_task_result_t *record = &s_task_history[job.id % APP_IR_TASK_HISTORY];
    memset(record, 0, sizeof(*record));
    record->id = job.id;
    record->state = IR_TASK_QUEUED;
    record->slot_mask = slot_mask;
    record->repeats = repeats;
    record->queued_us = esp_timer_get_time();
    strlcpy(record->origin, origin ? origin : "manual", sizeof(record->origin));
    strlcpy(record->result, "queued", sizeof(record->result));
    portEXIT_CRITICAL(&s_lock);
    strlcpy(job.origin, origin ? origin : "manual", sizeof(job.origin));
    if (xQueueSend(s_tx_queue, &job, 0) != pdTRUE) {
        update_task(job.id, IR_TASK_FAILED, "IR queue full", true);
        return ESP_ERR_NO_MEM;
    }
    *task_id = job.id;
    return ESP_OK;
}

esp_err_t ir_manager_get_task_result(uint32_t task_id, ir_task_result_t *result)
{
    if (!result || !task_id) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    ir_task_result_t *record = task_record(task_id);
    if (record) *result = *record;
    portEXIT_CRITICAL(&s_lock);
    return record ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void ir_manager_get_status(ir_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}
