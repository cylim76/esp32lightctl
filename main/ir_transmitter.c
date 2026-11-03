#include "ir_transmitter.h"

#include <stdlib.h>
#include "app_config.h"
#include "ir_storage.h"
#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ir_transmitter";
static rmt_channel_handle_t s_tx_channel;
static rmt_encoder_handle_t s_copy_encoder;

esp_err_t ir_transmitter_init(void)
{
    if (s_tx_channel) return ESP_OK;
    gpio_config_t idle = {
        .pin_bit_mask = 1ULL << APP_IR_TX_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&idle), TAG, "configure TX idle GPIO");
    gpio_set_level(APP_IR_TX_GPIO, !APP_IR_TX_ACTIVE_LEVEL);
    rmt_tx_channel_config_t config = {
        .gpio_num = APP_IR_TX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = APP_IR_RESOLUTION_HZ,
        .mem_block_symbols = 48,
        .trans_queue_depth = 2,
    };
    esp_err_t err = rmt_new_tx_channel(&config, &s_tx_channel);
    if (err != ESP_OK) return err;
    rmt_copy_encoder_config_t encoder_config = {};
    err = rmt_new_copy_encoder(&encoder_config, &s_copy_encoder);
    if (err != ESP_OK) goto fail;
    err = rmt_enable(s_tx_channel);
    if (err != ESP_OK) goto fail;
    ESP_LOGI(TAG, "RMT TX ready on GPIO%d; idle=%d, default carrier=%dHz/%d%%", APP_IR_TX_GPIO,
             !APP_IR_TX_ACTIVE_LEVEL, APP_IR_DEFAULT_CARRIER_HZ, APP_IR_DEFAULT_DUTY_PERCENT);
    return ESP_OK;
fail:
    if (s_copy_encoder) {
        rmt_del_encoder(s_copy_encoder);
        s_copy_encoder = NULL;
    }
    if (s_tx_channel) {
        rmt_del_channel(s_tx_channel);
        s_tx_channel = NULL;
    }
    gpio_set_level(APP_IR_TX_GPIO, !APP_IR_TX_ACTIVE_LEVEL);
    return err;
}

esp_err_t ir_transmitter_send(const ir_code_t *code, uint8_t repeats, uint32_t repeat_gap_ms)
{
    if (!s_tx_channel || !s_copy_encoder || !ir_code_valid(code)) return ESP_ERR_INVALID_STATE;
    if (repeats < 1 || repeats > 2 || repeat_gap_ms < 50 || repeat_gap_ms > 1000) return ESP_ERR_INVALID_ARG;
    size_t symbol_count = (code->duration_count + 1) / 2;
    rmt_symbol_word_t *symbols = calloc(symbol_count, sizeof(*symbols));
    if (!symbols) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < symbol_count; ++i) {
        size_t mark_index = i * 2;
        symbols[i].level0 = APP_IR_TX_ACTIVE_LEVEL;
        symbols[i].duration0 = code->durations_us[mark_index];
        symbols[i].level1 = !APP_IR_TX_ACTIVE_LEVEL;
        symbols[i].duration1 = mark_index + 1 < code->duration_count ? code->durations_us[mark_index + 1] : 1;
    }
    rmt_carrier_config_t carrier = {
        .frequency_hz = code->carrier_hz,
        .duty_cycle = code->duty_percent / 100.0f,
        .flags.polarity_active_low = !APP_IR_TX_ACTIVE_LEVEL,
        .flags.always_on = false,
    };
    esp_err_t err = rmt_apply_carrier(s_tx_channel, &carrier);
    rmt_transmit_config_t transmit = {
        .loop_count = 0,
        .flags.eot_level = !APP_IR_TX_ACTIVE_LEVEL,
        .flags.queue_nonblocking = false,
    };
    for (uint8_t repeat = 0; err == ESP_OK && repeat < repeats; ++repeat) {
        err = rmt_transmit(s_tx_channel, s_copy_encoder, symbols, symbol_count * sizeof(*symbols), &transmit);
        if (err == ESP_OK) err = rmt_tx_wait_all_done(s_tx_channel, 2000);
        gpio_set_level(APP_IR_TX_GPIO, !APP_IR_TX_ACTIVE_LEVEL);
        if (err == ESP_OK && repeat + 1 < repeats) vTaskDelay(pdMS_TO_TICKS(repeat_gap_ms));
    }
    free(symbols);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "IR RMT transmission completed: durations=%u repeats=%u carrier=%luHz/%u%%; AC state unconfirmed",
                 code->duration_count, repeats, (unsigned long)code->carrier_hz, code->duty_percent);
    } else {
        ESP_LOGE(TAG, "IR RMT transmission failed: %s", esp_err_to_name(err));
    }
    return err;
}
