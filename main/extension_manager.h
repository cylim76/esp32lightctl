#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define EXTENSION_HISTORY_COUNT 20
#define EXTENSION_ERROR_LENGTH 96

typedef enum {
    EXTENSION_MODULE_BH1750 = 0,
    EXTENSION_MODULE_SHT31,
    EXTENSION_MODULE_RAINMAKER,
    EXTENSION_MODULE_COUNT,
} extension_module_id_t;

typedef struct {
    bool included;
    bool enabled;
    bool running;
    bool online;
    bool data_valid;
    bool restart_required;
    int64_t last_success_uptime_ms;
    char last_error[EXTENSION_ERROR_LENGTH];
} extension_module_status_t;

typedef struct {
    int64_t uptime_ms;
    float value_1;
    float value_2;
} extension_sample_t;

typedef struct {
    uint16_t sample_period_seconds;
    uint16_t night_start_minute;
    uint16_t night_end_minute;
    float night_lux_threshold;
    uint8_t consecutive_anomaly_limit;
    uint8_t calibration_valid_mask;
    float calibration_lux[5];
    bool night_anomaly;
    float lux;
    float filtered_lux;
    size_t history_count;
    extension_sample_t history[EXTENSION_HISTORY_COUNT];
} extension_bh1750_view_t;

typedef struct {
    uint16_t sample_period_seconds;
    float temperature_c;
    float humidity_percent;
    const char *temperature_trend;
    size_t history_count;
    extension_sample_t history[EXTENSION_HISTORY_COUNT];
} extension_sht31_view_t;

typedef struct {
    bool claimed;
    bool mapping_in_progress;
    char node_id[40];
} extension_rainmaker_view_t;

typedef struct {
    extension_module_status_t modules[EXTENSION_MODULE_COUNT];
    extension_bh1750_view_t bh1750;
    extension_sht31_view_t sht31;
    extension_rainmaker_view_t rainmaker;
} extension_manager_snapshot_t;

/* Portable, non-sensitive extension configuration used by the versioned JSON
 * backup transaction. A bit is present only when the source firmware included
 * that module; absent modules never overwrite preserved target NVS data. */
typedef struct {
    uint8_t present_mask;
    uint8_t enabled_mask;
    uint16_t bh1750_sample_period_seconds;
    uint16_t bh1750_night_start_minute;
    uint16_t bh1750_night_end_minute;
    float bh1750_night_lux_threshold;
    uint8_t bh1750_consecutive_anomaly_limit;
    uint8_t bh1750_calibration_valid_mask;
    float bh1750_calibration_lux[5];
    uint16_t sht31_sample_period_seconds;
} extension_backup_config_t;

/* Optional module failures are diagnosed but never fail Core startup. */
esp_err_t extension_manager_start(void);
void extension_manager_get_snapshot(extension_manager_snapshot_t *snapshot);
esp_err_t extension_manager_set_enabled(extension_module_id_t module, bool enabled,
                                        bool *restart_required);
esp_err_t extension_manager_set_bh1750_config(const extension_bh1750_view_t *config);
esp_err_t extension_manager_record_bh1750_calibration(uint8_t scene);
esp_err_t extension_manager_set_sht31_config(uint16_t sample_period_seconds);
esp_err_t extension_manager_start_rainmaker_mapping(char *user_id, char *secret_key);
esp_err_t extension_manager_get_backup_config(extension_backup_config_t *config);
esp_err_t extension_manager_apply_backup_config(const extension_backup_config_t *config,
                                                size_t *applied, size_t *skipped,
                                                bool *restart_required);
const char *extension_manager_module_name(extension_module_id_t module);
