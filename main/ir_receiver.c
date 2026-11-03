#include "ir_receiver.h"

#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define IR_RX_SYMBOL_CAPACITY (APP_IR_MAX_DURATIONS / 2)

static const char *TAG = "ir_receiver";
static rmt_channel_handle_t s_rx_channel;
static QueueHandle_t s_rx_queue;
static rmt_symbol_word_t *s_symbols;

static bool duration_near(uint32_t actual, uint32_t expected)
{
    uint32_t margin = expected / 4;
    if (margin < 200) margin = 200;
    return actual + margin >= expected && actual <= expected + margin;
}

static void attempt_lg_decode(ir_code_t *code)
{
    code->protocol = IR_PROTOCOL_RAW;
    code->decoded_value = 0;
    if ((code->duration_count != 59 && code->duration_count != 60) ||
        !duration_near(code->durations_us[0], 8000) ||
        !duration_near(code->durations_us[1], 4000)) return;
    uint32_t data = 0;
    for (int bit = 0; bit < 28; ++bit) {
        uint16_t mark = code->durations_us[2 + bit * 2];
        uint16_t space = code->durations_us[3 + bit * 2];
        if (!duration_near(mark, 550)) return;
        if (duration_near(space, 1600)) data |= 1UL << bit;
        else if (!duration_near(space, 550)) return;
    }
    if (!duration_near(code->durations_us[58], 550)) return;
    code->decoded_value = data;
    code->protocol = IR_PROTOCOL_LG_LIKE_28;
    uint8_t checksum = data & 0x0F;
    uint32_t payload = data >> 4;
    uint8_t sum = 0;
    for (int nibble = 0; nibble < 6; ++nibble) sum += (payload >> (nibble * 4)) & 0x0F;
    if ((sum & 0x0F) == checksum) code->protocol = IR_PROTOCOL_LG_28_VALID_CHECKSUM;
}

static bool rx_done_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_data)
{
    (void)channel;
    BaseType_t wake = pdFALSE;
    xQueueSendFromISR((QueueHandle_t)user_data, edata, &wake);
    return wake == pdTRUE;
}

esp_err_t ir_receiver_open(void)
{
    if (s_rx_channel) return ESP_ERR_INVALID_STATE;
    s_symbols = calloc(IR_RX_SYMBOL_CAPACITY, sizeof(*s_symbols));
    if (!s_symbols) return ESP_ERR_NO_MEM;
    s_rx_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    if (!s_rx_queue) {
        free(s_symbols);
        s_symbols = NULL;
        return ESP_ERR_NO_MEM;
    }
    rmt_rx_channel_config_t config = {
        .gpio_num = APP_IR_RX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = APP_IR_RESOLUTION_HZ,
        .mem_block_symbols = 48,
    };
    esp_err_t err = rmt_new_rx_channel(&config, &s_rx_channel);
    if (err != ESP_OK) goto fail;
    rmt_rx_event_callbacks_t callbacks = {.on_recv_done = rx_done_callback};
    err = rmt_rx_register_event_callbacks(s_rx_channel, &callbacks, s_rx_queue);
    if (err != ESP_OK) goto fail;
    err = rmt_enable(s_rx_channel);
    if (err != ESP_OK) goto fail;
    gpio_set_pull_mode(APP_IR_RX_GPIO, GPIO_PULLUP_ONLY);
    ESP_LOGI(TAG, "RMT RX learning channel opened on GPIO%d; 38kHz is configured/estimated, not measured", APP_IR_RX_GPIO);
    return ESP_OK;
fail:
    ir_receiver_close();
    return err;
}

esp_err_t ir_receiver_capture(ir_code_t *code, uint32_t timeout_ms)
{
    if (!s_rx_channel || !s_symbols || !code) return ESP_ERR_INVALID_STATE;
    xQueueReset(s_rx_queue);
    memset(s_symbols, 0, IR_RX_SYMBOL_CAPACITY * sizeof(*s_symbols));
    rmt_receive_config_t config = {
        .signal_range_min_ns = APP_IR_RX_MIN_PULSE_NS,
        .signal_range_max_ns = APP_IR_RX_END_GAP_NS,
    };
    ESP_RETURN_ON_ERROR(rmt_receive(s_rx_channel, s_symbols,
                                    IR_RX_SYMBOL_CAPACITY * sizeof(*s_symbols), &config), TAG, "start RX");
    rmt_rx_done_event_data_t event;
    if (xQueueReceive(s_rx_queue, &event, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "IR learning capture timed out after %lu ms", (unsigned long)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    memset(code, 0, sizeof(*code));
    code->magic = IR_CODE_MAGIC;
    code->version = IR_CODE_VERSION;
    code->resolution_hz = APP_IR_RESOLUTION_HZ;
    code->carrier_hz = APP_IR_DEFAULT_CARRIER_HZ;
    code->duty_percent = APP_IR_DEFAULT_DUTY_PERCENT;
    code->mark_level = APP_IR_RX_MARK_LEVEL;
    code->carrier_measured = false;
    bool started = false;
    uint8_t previous_level = 0xFF;
    for (size_t i = 0; i < event.num_symbols; ++i) {
        const uint16_t durations[2] = {event.received_symbols[i].duration0, event.received_symbols[i].duration1};
        const uint8_t levels[2] = {event.received_symbols[i].level0, event.received_symbols[i].level1};
        for (int half = 0; half < 2; ++half) {
            uint16_t duration = durations[half];
            uint8_t level = levels[half];
            if (!duration) continue;
            if (!started) {
                if (level != APP_IR_RX_MARK_LEVEL) continue;
                started = true;
            }
            if (level == previous_level && code->duration_count) {
                uint32_t combined = code->durations_us[code->duration_count - 1] + duration;
                if (combined > UINT16_MAX) return ESP_ERR_INVALID_SIZE;
                code->durations_us[code->duration_count - 1] = combined;
            } else {
                if (code->duration_count >= APP_IR_MAX_DURATIONS) return ESP_ERR_INVALID_SIZE;
                code->durations_us[code->duration_count++] = duration;
                previous_level = level;
            }
            code->total_duration_us += duration;
        }
    }
    if (code->duration_count < 4 || code->total_duration_us > APP_IR_MAX_FRAME_US) return ESP_ERR_INVALID_SIZE;
    attempt_lg_decode(code);
    ESP_LOGI(TAG, "Captured IR: symbols=%u durations=%u total=%luus protocol=%s carrier=estimated-%luHz",
             (unsigned)event.num_symbols, code->duration_count, (unsigned long)code->total_duration_us,
             ir_protocol_name(code->protocol), (unsigned long)code->carrier_hz);
    return ESP_OK;
}

void ir_receiver_close(void)
{
    if (s_rx_channel) {
        rmt_disable(s_rx_channel);
        rmt_del_channel(s_rx_channel);
        s_rx_channel = NULL;
    }
    if (s_rx_queue) {
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
    }
    free(s_symbols);
    s_symbols = NULL;
    gpio_reset_pin(APP_IR_RX_GPIO);
    ESP_LOGI(TAG, "RMT RX learning resources released; receiver may be disconnected");
}
