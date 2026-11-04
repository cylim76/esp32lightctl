#include "device_control.h"

#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "stage_config.h"
#include "zigbee_manager.h"
#if LIGHTGW_HAS_IR
#include "ir_manager.h"
#endif

#define CONTROL_QUEUE_DEPTH 12
#define CONTROL_MAX_REFS (APP_MAX_ZIGBEE_DEVICES * 3)

typedef struct {
    uint32_t id;
    schedule_target_t target;
    uint16_t short_addr;
    int8_t channel;
    bool requested_on;
    uint8_t repeats;
} control_request_t;

typedef struct {
    uint16_t short_addr;
    uint8_t channel;
} light_ref_t;

typedef struct {
    uint32_t id;
    bool active;
    bool is_ir;
    bool requested_on;
    int64_t sent_us;
    int64_t deadline_us;
    uint32_t ir_task_id;
    uint8_t ref_count;
    light_ref_t refs[CONTROL_MAX_REFS];
} active_command_t;

static const char *TAG = "device_control";
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static device_command_result_t s_results[APP_CONTROL_COMMAND_CAPACITY];
static device_state_store_t s_store;
static active_command_t s_active[APP_CONTROL_COMMAND_CAPACITY];

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void state_update(uint32_t id, device_command_status_t status, device_actual_state_t actual,
                         bool terminal, const char *detail)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    device_state_update(&s_store, id, status, actual, terminal, detail, now_ms());
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "command=%lu status=%s terminal=%u detail=%s", (unsigned long)id,
             device_command_status_name(status), terminal, detail ? detail : "");
}

static void fail_request(uint32_t id, const char *detail)
{
    state_update(id, DEVICE_COMMAND_FAILED, DEVICE_ACTUAL_UNKNOWN, true, detail);
    active_command_t *active = &s_active[id % APP_CONTROL_COMMAND_CAPACITY];
    if (active->id == id) active->active = false;
}

static uint8_t target_light_mask(schedule_target_t target)
{
    if (target <= SCHEDULE_TARGET_LIGHT_RIGHT) return 1U << target;
    return target == SCHEDULE_TARGET_LIGHT_ALL ? 0x07 : 0;
}

static void dispatch_light(const control_request_t *request, active_command_t *active)
{
    uint8_t mask = request->channel >= 0 ? (1U << request->channel) : target_light_mask(request->target);
    zb_device_snapshot_t devices[APP_MAX_ZIGBEE_DEVICES];
    size_t count = zigbee_manager_snapshot(devices, APP_MAX_ZIGBEE_DEVICES);
    /* Cache timestamps are millisecond based; compare on the same time boundary. */
    active->sent_us = now_ms() * 1000;
    active->deadline_us = esp_timer_get_time() + (int64_t)APP_CONTROL_CONFIRM_TIMEOUT_MS * 1000;
    for (size_t i = 0; i < count; ++i) {
        if (!devices[i].supported || !devices[i].mapping_confirmed ||
            (request->short_addr != UINT16_MAX && devices[i].short_addr != request->short_addr)) continue;
        for (unsigned channel = 0; channel < 3; ++channel) {
            if (!(mask & (1U << channel)) || active->ref_count >= CONTROL_MAX_REFS) continue;
            if (zigbee_manager_set_channel(devices[i].short_addr, channel, request->requested_on) == ESP_OK) {
                active->refs[active->ref_count++] = (light_ref_t){devices[i].short_addr, channel};
            }
        }
    }
    if (!active->ref_count) {
        fail_request(request->id, "no mapped Zigbee endpoint accepted the command");
        return;
    }
    state_update(request->id, DEVICE_COMMAND_SENT, DEVICE_ACTUAL_UNKNOWN, false,
                 "Zigbee command sent; awaiting fresh endpoint feedback");
}

#if LIGHTGW_HAS_IR
static uint8_t target_ir_mask(schedule_target_t target)
{
    if (target == SCHEDULE_TARGET_AC_1) return 1;
    if (target == SCHEDULE_TARGET_AC_2) return 2;
    if (target == SCHEDULE_TARGET_AC_GROUP) return 3;
    return 0;
}
#endif

static void dispatch_request(const control_request_t *request)
{
    active_command_t *active = &s_active[request->id % APP_CONTROL_COMMAND_CAPACITY];
    memset(active, 0, sizeof(*active));
    active->id = request->id;
    active->active = true;
    active->requested_on = request->requested_on;
    if (request->target <= SCHEDULE_TARGET_LIGHT_ALL) {
        dispatch_light(request, active);
        return;
    }
#if LIGHTGW_HAS_IR
    uint8_t mask = target_ir_mask(request->target);
    active->is_ir = true;
    active->deadline_us = esp_timer_get_time() + (int64_t)APP_IR_RESULT_TIMEOUT_MS * 1000;
    esp_err_t err = ir_manager_submit_send(mask, request->repeats, "device_control", &active->ir_task_id);
    if (err == ESP_OK) {
        state_update(request->id, DEVICE_COMMAND_PENDING, DEVICE_ACTUAL_UNKNOWN, false,
                     "IR queued; waiting for RMT completion");
    } else {
        char detail[96];
        snprintf(detail, sizeof(detail), "IR enqueue failed: %s", esp_err_to_name(err));
        fail_request(request->id, detail);
    }
#else
    fail_request(request->id, "IR target is not available in this Stage");
#endif
}

static void refresh_active(active_command_t *active)
{
    if (!active->active) return;
    int64_t now = esp_timer_get_time();
    if (active->is_ir) {
#if LIGHTGW_HAS_IR
        ir_task_result_t ir;
        esp_err_t err = ir_manager_get_task_result(active->ir_task_id, &ir);
        if (err == ESP_OK && ir.state == IR_TASK_SENT) {
            state_update(active->id, DEVICE_COMMAND_SENT, DEVICE_ACTUAL_UNKNOWN, true,
                         "RMT transmission completed; AC physical state remains UNKNOWN");
            active->active = false;
        } else if (err == ESP_OK && ir.state == IR_TASK_FAILED) {
            fail_request(active->id, ir.result);
        } else if (err == ESP_OK && ir.state == IR_TASK_TRANSMITTING) {
            state_update(active->id, DEVICE_COMMAND_PENDING, DEVICE_ACTUAL_UNKNOWN, false,
                         "RMT transmission in progress");
        } else if (now > active->deadline_us) {
            fail_request(active->id, "IR result timeout");
        }
#endif
        return;
    }
    bool all_confirmed = active->ref_count > 0;
    bool read_timeout = false;
    for (size_t i = 0; i < active->ref_count; ++i) {
        zb_channel_status_t channel = {0};
        if (zigbee_manager_get_channel_status(active->refs[i].short_addr, active->refs[i].channel, &channel) != ESP_OK ||
            !channel.valid || channel.updated_us < active->sent_us ||
            channel.state != (active->requested_on ? ZB_CHANNEL_ON : ZB_CHANNEL_OFF)) all_confirmed = false;
        if (channel.read_timed_out) read_timeout = true;
    }
    if (all_confirmed) {
        state_update(active->id, DEVICE_COMMAND_CONFIRMED,
                     active->requested_on ? DEVICE_ACTUAL_ON : DEVICE_ACTUAL_OFF, true,
                     "fresh feedback confirmed every targeted device endpoint");
        active->active = false;
    } else if (now > active->deadline_us) {
        fail_request(active->id, read_timeout ? "Zigbee readback timeout; stale cache rejected" :
                                               "fresh Zigbee confirmation not received before timeout");
    }
}

static void control_task(void *arg)
{
    (void)arg;
    for (;;) {
        control_request_t request;
        if (xQueueReceive(s_queue, &request, pdMS_TO_TICKS(50)) == pdTRUE) dispatch_request(&request);
        for (size_t i = 0; i < APP_CONTROL_COMMAND_CAPACITY; ++i) refresh_active(&s_active[i]);
    }
}

esp_err_t device_control_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    device_state_store_init(&s_store, s_results, APP_CONTROL_COMMAND_CAPACITY);
    memset(s_active, 0, sizeof(s_active));
    s_queue = xQueueCreate(CONTROL_QUEUE_DEPTH, sizeof(control_request_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    return xTaskCreate(control_task, "device_control", 6144, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t submit(control_request_t *request, const char *source, uint32_t *command_id)
{
    if (!request || !command_id || !s_queue || !s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    request->id = device_state_create(&s_store, request->target, request->short_addr, request->channel,
                                      request->requested_on, source, now_ms());
    xSemaphoreGive(s_lock);
    if (!request->id) return ESP_ERR_NO_MEM;
    if (xQueueSend(s_queue, request, 0) != pdTRUE) {
        state_update(request->id, DEVICE_COMMAND_FAILED, DEVICE_ACTUAL_UNKNOWN, true, "device control queue full");
        return ESP_ERR_NO_MEM;
    }
    *command_id = request->id;
    return ESP_OK;
}

esp_err_t device_control_submit_light(uint16_t short_addr, unsigned channel, bool on,
                                      const char *source, uint32_t *command_id)
{
    if (channel > 2) return ESP_ERR_INVALID_ARG;
    control_request_t request = {
        .target = (schedule_target_t)channel, .short_addr = short_addr,
        .channel = channel, .requested_on = on, .repeats = 1,
    };
    return submit(&request, source, command_id);
}

esp_err_t device_control_submit_target(schedule_target_t target, const char *source,
                                       uint32_t *command_id)
{
    return device_control_submit_target_repeated(target, 1, source, command_id);
}

esp_err_t device_control_submit_target_state(schedule_target_t target, bool on,
                                             const char *source, uint32_t *command_id)
{
    if (target >= APP_SCHEDULE_TARGET_COUNT) return ESP_ERR_INVALID_ARG;
    /* An IR air-conditioner target only has a learned, explicit OFF code. */
    if (on && target >= SCHEDULE_TARGET_AC_1) return ESP_ERR_NOT_SUPPORTED;
    control_request_t request = {
        .target = target, .short_addr = UINT16_MAX, .channel = -1,
        .requested_on = on, .repeats = 1,
    };
    return submit(&request, source, command_id);
}

esp_err_t device_control_submit_target_repeated(schedule_target_t target, uint8_t repeats,
                                                const char *source, uint32_t *command_id)
{
    if (target >= APP_SCHEDULE_TARGET_COUNT || repeats < 1 || repeats > 2) return ESP_ERR_INVALID_ARG;
    control_request_t request = {
        .target = target, .short_addr = UINT16_MAX, .channel = -1, .requested_on = false,
        .repeats = repeats,
    };
    return submit(&request, source, command_id);
}

esp_err_t device_control_get_result(uint32_t command_id, device_command_result_t *result)
{
    if (!result || !command_id || !s_lock) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = device_state_get(&s_store, command_id, result);
    xSemaphoreGive(s_lock);
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}
