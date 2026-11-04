#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define BH1750_EXTENSION_CONFIG_NAMESPACE "bh1750_ext"
#define BH1750_EXTENSION_CONFIG_VERSION   1
#define BH1750_EXTENSION_HISTORY_COUNT    20
#define BH1750_EXTENSION_CALIBRATION_COUNT 5

typedef enum {
    BH1750_CAL_ALL_OFF = 0,
    BH1750_CAL_LEFT_ON,
    BH1750_CAL_CENTER_ON,
    BH1750_CAL_RIGHT_ON,
    BH1750_CAL_ALL_ON,
} bh1750_calibration_scene_t;

typedef struct {
    uint16_t version;
    uint16_t sample_period_seconds;
    uint16_t night_start_minute;
    uint16_t night_end_minute;
    float night_lux_threshold;
    uint8_t consecutive_anomaly_limit;
    uint8_t calibration_valid_mask;
    float calibration_lux[BH1750_EXTENSION_CALIBRATION_COUNT];
} bh1750_extension_config_t;

typedef struct {
    int64_t uptime_ms;
    float lux;
    float filtered_lux;
} bh1750_sample_t;

typedef struct {
    bool running;
    bool present;
    bool online;
    bool data_valid;
    bool night_anomaly;
    uint8_t i2c_address;
    float lux;
    float filtered_lux;
    uint32_t successful_samples;
    uint32_t consecutive_errors;
    int64_t last_success_uptime_ms;
    char last_error[96];
    size_t history_count;
    bh1750_sample_t history[BH1750_EXTENSION_HISTORY_COUNT];
} bh1750_extension_status_t;

esp_err_t bh1750_extension_start(void);
esp_err_t bh1750_extension_stop(void);
void bh1750_extension_get_status(bh1750_extension_status_t *status);
void bh1750_extension_get_config(bh1750_extension_config_t *config);
esp_err_t bh1750_extension_set_config(const bh1750_extension_config_t *config);
esp_err_t bh1750_extension_record_calibration(bh1750_calibration_scene_t scene);
