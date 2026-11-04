#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SHT31_EXTENSION_CONFIG_NAMESPACE "sht31_ext"
#define SHT31_EXTENSION_CONFIG_VERSION   1
#define SHT31_EXTENSION_HISTORY_COUNT    20

typedef enum {
    SHT31_TREND_UNKNOWN = 0,
    SHT31_TREND_STABLE,
    SHT31_TREND_RISING,
    SHT31_TREND_FALLING,
} sht31_temperature_trend_t;

typedef struct {
    uint16_t version;
    uint16_t sample_period_seconds;
} sht31_extension_config_t;

typedef struct {
    int64_t uptime_ms;
    float temperature_c;
    float humidity_percent;
} sht31_sample_t;

typedef struct {
    bool running;
    bool present;
    bool online;
    bool data_valid;
    uint8_t i2c_address;
    float temperature_c;
    float humidity_percent;
    sht31_temperature_trend_t temperature_trend;
    uint32_t successful_samples;
    uint32_t consecutive_errors;
    int64_t last_success_uptime_ms;
    char last_error[96];
    size_t history_count;
    sht31_sample_t history[SHT31_EXTENSION_HISTORY_COUNT];
} sht31_extension_status_t;

esp_err_t sht31_extension_start(void);
esp_err_t sht31_extension_stop(void);
void sht31_extension_get_status(sht31_extension_status_t *status);
void sht31_extension_get_config(sht31_extension_config_t *config);
esp_err_t sht31_extension_read_config(sht31_extension_config_t *config);
esp_err_t sht31_extension_set_config(const sht31_extension_config_t *config);
