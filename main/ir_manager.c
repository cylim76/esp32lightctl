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
    uint8_t slot_mask;
    uint8_t repeats;
    char origin[16];
} ir_tx_job_t;

static const char *TAG = "ir_manager";
static QueueHandle_t s_tx_queue;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static ir_status_t s_status;
static ir_code_t s_candidate;
static int64_t s_last_schedule_minute = -1;

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

static int64_t minute_key(const struct tm *local)
{
    return (((int64_t)local->tm_year * 367 + local->tm_yday) * 24 + local->tm_hour) * 60 + local->tm_min;
}

static void evaluate_schedule(void)
{
    ir_schedule_t schedule;
    portENTER_CRITICAL(&s_lock);
    schedule = s_status.schedule;
    portEXIT_CRITICAL(&s_lock);
    if (!schedule.enabled) {
        portENTER_CRITICAL(&s_lock);
        strlcpy(s_status.schedule_skip_reason, "disabled", sizeof(s_status.schedule_skip_reason));
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    const app_settings_t *settings = settings_get();
    for (size_t i = 0; i < settings->schedule_count; ++i) {
        if (settings->schedules[i].target >= SCHEDULE_TARGET_AC_1) {
            portENTER_CRITICAL(&s_lock);
            strlcpy(s_status.schedule_skip_reason, "legacy IR schedule superseded by per-device AC schedules",
                    sizeof(s_status.schedule_skip_reason));
            portEXIT_CRITICAL(&s_lock);
            return;
        }
    }
    const char *reason = NULL;
    if (!time_manager_is_valid()) reason = "skipped: trusted system time unavailable";
    if (reason) {
        portENTER_CRITICAL(&s_lock);
        strlcpy(s_status.schedule_skip_reason, reason, sizeof(s_status.schedule_skip_reason));
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    time_t now;
    struct tm local;
    time(&now);
    localtime_r(&now, &local);
    int64_t key = minute_key(&local);
    bool due = false;
    if (schedule.use_light_schedules) {
        for (size_t i = 0; i < settings->schedule_count; ++i) {
            const app_schedule_entry_t *entry = &settings->schedules[i];
            if (entry->enabled && entry->target <= SCHEDULE_TARGET_LIGHT_ALL &&
                (entry->days & (1U << local.tm_wday)) &&
                entry->hour == local.tm_hour && entry->minute == local.tm_min) {
                due = true;
                break;
            }
        }
    } else {
        due = (schedule.days & (1U << local.tm_wday)) && schedule.hour == local.tm_hour &&
              schedule.minute == local.tm_min;
    }
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_status.schedule_skip_reason, due ? "due: queued" : "waiting for configured time",
            sizeof(s_status.schedule_skip_reason));
    portEXIT_CRITICAL(&s_lock);
    if (due && key == s_last_schedule_minute) {
        portENTER_CRITICAL(&s_lock);
        strlcpy(s_status.schedule_skip_reason, "already triggered this minute", sizeof(s_status.schedule_skip_reason));
        portEXIT_CRITICAL(&s_lock);
    } else if (due) {
        esp_err_t err = ir_manager_enqueue_send(schedule.slot_mask, 1, "schedule");
        if (err == ESP_OK) s_last_schedule_minute = key;
        else {
            portENTER_CRITICAL(&s_lock);
            snprintf(s_status.schedule_skip_reason, sizeof(s_status.schedule_skip_reason),
                     "schedule queue failed: %s", esp_err_to_name(err));
            portEXIT_CRITICAL(&s_lock);
        }
    }
}

static void tx_task(void *arg)
{
    (void)arg;
    TickType_t last_schedule_check = 0;
    for (;;) {
        ir_tx_job_t job;
        if (xQueueReceive(s_tx_queue, &job, pdMS_TO_TICKS(1000)) == pdTRUE) {
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
            portENTER_CRITICAL(&s_lock);
            s_status.transmitting = false;
            portEXIT_CRITICAL(&s_lock);
        }
        TickType_t now = xTaskGetTickCount();
        if (now - last_schedule_check >= pdMS_TO_TICKS(10 * 1000)) {
            last_schedule_check = now;
            evaluate_schedule();
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

esp_err_t ir_manager_enqueue_send(uint8_t slot_mask, uint8_t repeats, const char *origin)
{
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
    strlcpy(job.origin, origin ? origin : "manual", sizeof(job.origin));
    return xQueueSend(s_tx_queue, &job, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ir_manager_save_schedule(const ir_schedule_t *schedule)
{
    esp_err_t err = ir_storage_save_schedule(schedule);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_status.schedule = *schedule;
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}

void ir_manager_get_status(ir_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}
