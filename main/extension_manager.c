#include "extension_manager.h"

#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "diagnostics.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "stage_config.h"
#include "storage_manager.h"
#include "esp_timer.h"

#if LIGHTGW_HAS_BH1750_EXTENSION
#include "bh1750_extension.h"
#endif
#if LIGHTGW_HAS_SHT31_EXTENSION
#include "sht31_extension.h"
#endif
#if LIGHTGW_HAS_RAINMAKER_EXTENSION
#include "device_control.h"
#include "rainmaker_extension.h"
#include "zigbee_manager.h"
#endif

#define EXTENSION_MANAGER_NAMESPACE "module_mgr"
#define EXTENSION_MANAGER_VERSION 1

static const char *TAG = "extensions";
static bool s_started;
static bool s_enabled[EXTENSION_MODULE_COUNT];
static bool s_restart_required[EXTENSION_MODULE_COUNT];
static char s_manager_error[EXTENSION_MODULE_COUNT][EXTENSION_ERROR_LENGTH];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static bool module_included(extension_module_id_t module)
{
    switch (module) {
    case EXTENSION_MODULE_BH1750: return LIGHTGW_HAS_BH1750_EXTENSION;
    case EXTENSION_MODULE_SHT31: return LIGHTGW_HAS_SHT31_EXTENSION;
    case EXTENSION_MODULE_RAINMAKER: return LIGHTGW_HAS_RAINMAKER_EXTENSION;
    default: return false;
    }
}

const char *extension_manager_module_name(extension_module_id_t module)
{
    switch (module) {
    case EXTENSION_MODULE_BH1750: return "bh1750";
    case EXTENSION_MODULE_SHT31: return "sht31";
    case EXTENSION_MODULE_RAINMAKER: return "rainmaker";
    default: return "invalid";
    }
}

static const char *module_partition(extension_module_id_t module)
{
    return module == EXTENSION_MODULE_RAINMAKER ? APP_CLOUD_STORAGE_PARTITION :
                                                  APP_SENSOR_STORAGE_PARTITION;
}

static const char *module_key(extension_module_id_t module)
{
    switch (module) {
    case EXTENSION_MODULE_BH1750: return "bh_enabled";
    case EXTENSION_MODULE_SHT31: return "sht_enabled";
    case EXTENSION_MODULE_RAINMAKER: return "rm_enabled";
    default: return "invalid";
    }
}

static void manager_error(extension_module_id_t module, esp_err_t err, const char *operation)
{
    portENTER_CRITICAL(&s_lock);
    snprintf(s_manager_error[module], sizeof(s_manager_error[module]), "%s: %s",
             operation, esp_err_to_name(err));
    portEXIT_CRITICAL(&s_lock);
    diagnostics_set_error(TAG, "%s %s failed: %s; Core continues",
                          extension_manager_module_name(module), operation, esp_err_to_name(err));
    ESP_LOGW(TAG, "%s %s failed: %s", extension_manager_module_name(module),
             operation, esp_err_to_name(err));
}

static esp_err_t load_enabled(extension_module_id_t module, bool *enabled)
{
    *enabled = true;
    const char *partition = module_partition(module);
    esp_err_t err = storage_manager_init_optional_partition(partition);
    if (err != ESP_OK) return err;
    nvs_handle_t nvs = 0;
    err = nvs_open_from_partition(partition, EXTENSION_MANAGER_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    uint8_t stored = 1;
    err = nvs_get_u8(nvs, module_key(module), &stored);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (stored > 1) return ESP_ERR_INVALID_CRC;
    *enabled = stored != 0;
    return ESP_OK;
}

static esp_err_t persist_enabled(extension_module_id_t module, bool enabled)
{
    const char *partition = module_partition(module);
    esp_err_t err = storage_manager_init_optional_partition(partition);
    if (err != ESP_OK) return err;
    nvs_handle_t nvs = 0;
    err = nvs_open_from_partition(partition, EXTENSION_MANAGER_NAMESPACE,
                                  NVS_READWRITE, &nvs);
    if (err == ESP_OK) err = nvs_set_u8(nvs, "version", EXTENSION_MANAGER_VERSION);
    if (err == ESP_OK) err = nvs_set_u8(nvs, module_key(module), enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(nvs);
    if (nvs) nvs_close(nvs);
    storage_manager_record_write(partition, module_key(module), err, true);
    return err;
}

#if LIGHTGW_HAS_RAINMAKER_EXTENSION
static esp_err_t cloud_submit_target(uint8_t target, bool on, const char *source,
                                     uint32_t *command_id)
{
    if (target >= APP_SCHEDULE_TARGET_COUNT) return ESP_ERR_INVALID_ARG;
    return device_control_submit_target_state((schedule_target_t)target, on, source, command_id);
}

static esp_err_t cloud_get_command_result(uint32_t command_id,
                                          rainmaker_command_result_t *result)
{
    if (!result) return ESP_ERR_INVALID_ARG;
    device_command_result_t core;
    esp_err_t err = device_control_get_result(command_id, &core);
    if (err != ESP_OK) return err;
    memset(result, 0, sizeof(*result));
    result->requested_on = core.requested_on;
    strlcpy(result->detail, core.detail, sizeof(result->detail));
    if (!core.terminal) result->state = RAINMAKER_COMMAND_PENDING;
    else if (core.status == DEVICE_COMMAND_CONFIRMED) result->state = RAINMAKER_COMMAND_CONFIRMED;
    else if (core.status == DEVICE_COMMAND_SENT && core.actual_state == DEVICE_ACTUAL_UNKNOWN) {
        result->state = RAINMAKER_COMMAND_SENT_UNCONFIRMED;
    } else result->state = RAINMAKER_COMMAND_FAILED;
    return ESP_OK;
}

static esp_err_t cloud_get_light_state(uint8_t target, rainmaker_light_state_t *state)
{
    if (!state || target > SCHEDULE_TARGET_LIGHT_RIGHT) return ESP_ERR_INVALID_ARG;
    memset(state, 0, sizeof(*state));
    zb_device_snapshot_t devices[APP_MAX_ZIGBEE_DEVICES];
    size_t count = zigbee_manager_snapshot(devices, APP_MAX_ZIGBEE_DEVICES);
    int64_t now_us = esp_timer_get_time();
    bool found = false;
    bool aggregate_on = false;
    int64_t newest_us = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!devices[i].supported || !devices[i].mapping_confirmed || !devices[i].online) continue;
        zb_channel_status_t channel = {0};
        if (zigbee_manager_get_channel_status(devices[i].short_addr, target, &channel) != ESP_OK ||
            !channel.valid || channel.read_timed_out ||
            now_us - channel.updated_us > (int64_t)APP_ZIGBEE_STATE_MAX_AGE_MS * 1000) continue;
        found = true;
        aggregate_on |= channel.state == ZB_CHANNEL_ON;
        if (channel.updated_us > newest_us) newest_us = channel.updated_us;
    }
    state->valid = found;
    state->on = aggregate_on;
    state->updated_uptime_ms = newest_us / 1000;
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static void cloud_get_environment(rainmaker_environment_t *environment)
{
    if (!environment) return;
    memset(environment, 0, sizeof(*environment));
#if LIGHTGW_HAS_BH1750_EXTENSION
    bh1750_extension_status_t bh;
    bh1750_extension_get_status(&bh);
    environment->lux_valid = bh.running && bh.online && bh.data_valid;
    environment->lux = bh.filtered_lux;
#endif
#if LIGHTGW_HAS_SHT31_EXTENSION
    sht31_extension_status_t sht;
    sht31_extension_get_status(&sht);
    environment->climate_valid = sht.running && sht.online && sht.data_valid;
    environment->temperature_c = sht.temperature_c;
    environment->humidity_percent = sht.humidity_percent;
#endif
}
#endif

static esp_err_t start_module(extension_module_id_t module)
{
    switch (module) {
#if LIGHTGW_HAS_BH1750_EXTENSION
    case EXTENSION_MODULE_BH1750: return bh1750_extension_start();
#endif
#if LIGHTGW_HAS_SHT31_EXTENSION
    case EXTENSION_MODULE_SHT31: return sht31_extension_start();
#endif
#if LIGHTGW_HAS_RAINMAKER_EXTENSION
    case EXTENSION_MODULE_RAINMAKER: {
        const rainmaker_control_api_t api = {
            .submit_target = cloud_submit_target,
            .get_command_result = cloud_get_command_result,
            .get_light_state = cloud_get_light_state,
            .get_environment = cloud_get_environment,
            .environment_available = LIGHTGW_HAS_BH1750_EXTENSION || LIGHTGW_HAS_SHT31_EXTENSION,
        };
        return rainmaker_extension_start(&api);
    }
#endif
    default: return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t stop_module(extension_module_id_t module, bool *restart_required)
{
    *restart_required = false;
    switch (module) {
#if LIGHTGW_HAS_BH1750_EXTENSION
    case EXTENSION_MODULE_BH1750: return bh1750_extension_stop();
#endif
#if LIGHTGW_HAS_SHT31_EXTENSION
    case EXTENSION_MODULE_SHT31: return sht31_extension_stop();
#endif
#if LIGHTGW_HAS_RAINMAKER_EXTENSION
    case EXTENSION_MODULE_RAINMAKER: {
        esp_err_t err = rainmaker_extension_stop();
        if (err == ESP_ERR_NOT_SUPPORTED) {
            *restart_required = true;
            return ESP_OK;
        }
        return err;
    }
#endif
    default: return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t extension_manager_start(void)
{
    if (s_started) return ESP_OK;
    s_started = true;
    for (extension_module_id_t module = 0; module < EXTENSION_MODULE_COUNT; ++module) {
        strlcpy(s_manager_error[module], "none", sizeof(s_manager_error[module]));
        if (!module_included(module)) continue;
        bool enabled = true;
        esp_err_t err = load_enabled(module, &enabled);
        if (err != ESP_OK) {
            manager_error(module, err, "config load; safe default enabled");
            enabled = true;
        }
        s_enabled[module] = enabled;
        if (!enabled) {
            ESP_LOGI(TAG, "%s included but disabled by user", extension_manager_module_name(module));
            continue;
        }
        err = start_module(module);
        if (err != ESP_OK) manager_error(module, err, "start");
    }
    return ESP_OK;
}

esp_err_t extension_manager_set_enabled(extension_module_id_t module, bool enabled,
                                        bool *restart_required)
{
    if (module < 0 || module >= EXTENSION_MODULE_COUNT || !restart_required) {
        return ESP_ERR_INVALID_ARG;
    }
    *restart_required = false;
    if (!module_included(module)) return ESP_ERR_NOT_SUPPORTED;
    portENTER_CRITICAL(&s_lock);
    bool unchanged = s_enabled[module] == enabled;
    portEXIT_CRITICAL(&s_lock);
    if (unchanged) return ESP_OK;
    esp_err_t err = persist_enabled(module, enabled);
    if (err != ESP_OK) {
        manager_error(module, err, "persist enable state");
        return err;
    }
    portENTER_CRITICAL(&s_lock);
    s_enabled[module] = enabled;
    strlcpy(s_manager_error[module], "none", sizeof(s_manager_error[module]));
    portEXIT_CRITICAL(&s_lock);
    if (enabled) {
        err = start_module(module);
    } else {
        err = stop_module(module, restart_required);
    }
    if (err != ESP_OK) manager_error(module, err, enabled ? "runtime start" : "runtime stop");
    portENTER_CRITICAL(&s_lock);
    s_restart_required[module] = *restart_required;
    portEXIT_CRITICAL(&s_lock);
    diagnostics_record_memory_event(enabled ? "extension_enabled" : "extension_disabled");
    return err;
}

#if LIGHTGW_HAS_SHT31_EXTENSION
static const char *trend_name(int trend)
{
    switch (trend) {
    case 1: return "STABLE";
    case 2: return "RISING";
    case 3: return "FALLING";
    default: return "UNKNOWN";
    }
}
#endif

void extension_manager_get_snapshot(extension_manager_snapshot_t *snapshot)
{
    if (!snapshot) return;
    memset(snapshot, 0, sizeof(*snapshot));
    for (extension_module_id_t module = 0; module < EXTENSION_MODULE_COUNT; ++module) {
        extension_module_status_t *status = &snapshot->modules[module];
        status->included = module_included(module);
        portENTER_CRITICAL(&s_lock);
        status->enabled = s_enabled[module];
        status->restart_required = s_restart_required[module];
        strlcpy(status->last_error, s_manager_error[module], sizeof(status->last_error));
        portEXIT_CRITICAL(&s_lock);
    }
#if LIGHTGW_HAS_BH1750_EXTENSION
    bh1750_extension_status_t bh_status;
    bh1750_extension_config_t bh_config;
    bh1750_extension_get_status(&bh_status);
    bh1750_extension_get_config(&bh_config);
    extension_module_status_t *bh = &snapshot->modules[EXTENSION_MODULE_BH1750];
    bh->running = bh_status.running;
    bh->online = bh_status.online;
    bh->data_valid = bh_status.data_valid;
    bh->last_success_uptime_ms = bh_status.last_success_uptime_ms;
    if (strcmp(bh_status.last_error, "none") != 0) strlcpy(bh->last_error, bh_status.last_error, sizeof(bh->last_error));
    snapshot->bh1750.sample_period_seconds = bh_config.sample_period_seconds;
    snapshot->bh1750.night_start_minute = bh_config.night_start_minute;
    snapshot->bh1750.night_end_minute = bh_config.night_end_minute;
    snapshot->bh1750.night_lux_threshold = bh_config.night_lux_threshold;
    snapshot->bh1750.consecutive_anomaly_limit = bh_config.consecutive_anomaly_limit;
    snapshot->bh1750.calibration_valid_mask = bh_config.calibration_valid_mask;
    memcpy(snapshot->bh1750.calibration_lux, bh_config.calibration_lux,
           sizeof(snapshot->bh1750.calibration_lux));
    snapshot->bh1750.night_anomaly = bh_status.night_anomaly;
    snapshot->bh1750.lux = bh_status.lux;
    snapshot->bh1750.filtered_lux = bh_status.filtered_lux;
    snapshot->bh1750.history_count = bh_status.history_count;
    for (size_t i = 0; i < bh_status.history_count; ++i) {
        snapshot->bh1750.history[i] = (extension_sample_t){
            .uptime_ms = bh_status.history[i].uptime_ms,
            .value_1 = bh_status.history[i].lux,
            .value_2 = bh_status.history[i].filtered_lux,
        };
    }
#endif
#if LIGHTGW_HAS_SHT31_EXTENSION
    sht31_extension_status_t sht_status;
    sht31_extension_config_t sht_config;
    sht31_extension_get_status(&sht_status);
    sht31_extension_get_config(&sht_config);
    extension_module_status_t *sht = &snapshot->modules[EXTENSION_MODULE_SHT31];
    sht->running = sht_status.running;
    sht->online = sht_status.online;
    sht->data_valid = sht_status.data_valid;
    sht->last_success_uptime_ms = sht_status.last_success_uptime_ms;
    if (strcmp(sht_status.last_error, "none") != 0) strlcpy(sht->last_error, sht_status.last_error, sizeof(sht->last_error));
    snapshot->sht31.sample_period_seconds = sht_config.sample_period_seconds;
    snapshot->sht31.temperature_c = sht_status.temperature_c;
    snapshot->sht31.humidity_percent = sht_status.humidity_percent;
    snapshot->sht31.temperature_trend = trend_name(sht_status.temperature_trend);
    snapshot->sht31.history_count = sht_status.history_count;
    for (size_t i = 0; i < sht_status.history_count; ++i) {
        snapshot->sht31.history[i] = (extension_sample_t){
            .uptime_ms = sht_status.history[i].uptime_ms,
            .value_1 = sht_status.history[i].temperature_c,
            .value_2 = sht_status.history[i].humidity_percent,
        };
    }
#endif
#if LIGHTGW_HAS_RAINMAKER_EXTENSION
    rainmaker_extension_status_t cloud_status;
    rainmaker_extension_get_status(&cloud_status);
    extension_module_status_t *cloud = &snapshot->modules[EXTENSION_MODULE_RAINMAKER];
    cloud->running = cloud_status.running;
    cloud->online = cloud_status.online;
    cloud->last_success_uptime_ms = cloud_status.last_success_uptime_ms;
    snapshot->rainmaker.claimed = cloud_status.claimed;
    snapshot->rainmaker.mapping_in_progress = cloud_status.mapping_in_progress;
    strlcpy(snapshot->rainmaker.node_id, cloud_status.node_id,
            sizeof(snapshot->rainmaker.node_id));
    if (strcmp(cloud_status.last_error, "none") != 0) {
        strlcpy(cloud->last_error, cloud_status.last_error, sizeof(cloud->last_error));
    }
#endif
}

esp_err_t extension_manager_start_rainmaker_mapping(char *user_id, char *secret_key)
{
#if LIGHTGW_HAS_RAINMAKER_EXTENSION
    if (!user_id || !secret_key) return ESP_ERR_INVALID_ARG;
    return rainmaker_extension_start_user_mapping(user_id, secret_key);
#else
    (void)user_id;
    (void)secret_key;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t extension_manager_set_bh1750_config(const extension_bh1750_view_t *config)
{
#if LIGHTGW_HAS_BH1750_EXTENSION
    if (!config) return ESP_ERR_INVALID_ARG;
    bh1750_extension_config_t current;
    esp_err_t err = bh1750_extension_read_config(&current);
    if (err != ESP_OK) return err;
    current.sample_period_seconds = config->sample_period_seconds;
    current.night_start_minute = config->night_start_minute;
    current.night_end_minute = config->night_end_minute;
    current.night_lux_threshold = config->night_lux_threshold;
    current.consecutive_anomaly_limit = config->consecutive_anomaly_limit;
    return bh1750_extension_set_config(&current);
#else
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t extension_manager_record_bh1750_calibration(uint8_t scene)
{
#if LIGHTGW_HAS_BH1750_EXTENSION
    return bh1750_extension_record_calibration((bh1750_calibration_scene_t)scene);
#else
    (void)scene;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t extension_manager_set_sht31_config(uint16_t sample_period_seconds)
{
#if LIGHTGW_HAS_SHT31_EXTENSION
    sht31_extension_config_t current;
    esp_err_t err = sht31_extension_read_config(&current);
    if (err != ESP_OK) return err;
    current.sample_period_seconds = sample_period_seconds;
    return sht31_extension_set_config(&current);
#else
    (void)sample_period_seconds;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t extension_manager_get_backup_config(extension_backup_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    memset(config, 0, sizeof(*config));
    extension_manager_snapshot_t snapshot;
    extension_manager_get_snapshot(&snapshot);
    for (extension_module_id_t module = 0; module < EXTENSION_MODULE_COUNT; ++module) {
        if (!snapshot.modules[module].included) continue;
        config->present_mask |= 1U << module;
        if (snapshot.modules[module].enabled) config->enabled_mask |= 1U << module;
    }
#if LIGHTGW_HAS_BH1750_EXTENSION
    bh1750_extension_config_t bh_config;
    esp_err_t bh_err = bh1750_extension_read_config(&bh_config);
    if (bh_err != ESP_OK) return bh_err;
    config->bh1750_sample_period_seconds = bh_config.sample_period_seconds;
    config->bh1750_night_start_minute = bh_config.night_start_minute;
    config->bh1750_night_end_minute = bh_config.night_end_minute;
    config->bh1750_night_lux_threshold = bh_config.night_lux_threshold;
    config->bh1750_consecutive_anomaly_limit = bh_config.consecutive_anomaly_limit;
    config->bh1750_calibration_valid_mask = bh_config.calibration_valid_mask;
    memcpy(config->bh1750_calibration_lux, bh_config.calibration_lux,
           sizeof(config->bh1750_calibration_lux));
#endif
#if LIGHTGW_HAS_SHT31_EXTENSION
    sht31_extension_config_t sht_config;
    esp_err_t sht_err = sht31_extension_read_config(&sht_config);
    if (sht_err != ESP_OK) return sht_err;
    config->sht31_sample_period_seconds = sht_config.sample_period_seconds;
#endif
    return ESP_OK;
}

esp_err_t extension_manager_apply_backup_config(const extension_backup_config_t *config,
                                                size_t *applied, size_t *skipped,
                                                bool *restart_required)
{
    if (!config || !applied || !skipped || !restart_required ||
        (config->present_mask & ~((1U << EXTENSION_MODULE_COUNT) - 1U)) ||
        (config->enabled_mask & ~config->present_mask)) return ESP_ERR_INVALID_ARG;
    *applied = 0;
    *skipped = 0;
    *restart_required = false;
    esp_err_t err = ESP_OK;
    for (extension_module_id_t module = 0; module < EXTENSION_MODULE_COUNT; ++module) {
        if (!(config->present_mask & (1U << module))) continue;
        if (!module_included(module)) {
            (*skipped)++;
            continue;
        }
        err = storage_manager_init_optional_partition(module_partition(module));
        if (err != ESP_OK) return err;
#if LIGHTGW_HAS_BH1750_EXTENSION
        if (module == EXTENSION_MODULE_BH1750) {
            bh1750_extension_config_t imported = {
                .version = BH1750_EXTENSION_CONFIG_VERSION,
                .sample_period_seconds = config->bh1750_sample_period_seconds,
                .night_start_minute = config->bh1750_night_start_minute,
                .night_end_minute = config->bh1750_night_end_minute,
                .night_lux_threshold = config->bh1750_night_lux_threshold,
                .consecutive_anomaly_limit = config->bh1750_consecutive_anomaly_limit,
                .calibration_valid_mask = config->bh1750_calibration_valid_mask,
            };
            memcpy(imported.calibration_lux, config->bh1750_calibration_lux,
                   sizeof(imported.calibration_lux));
            err = bh1750_extension_set_config(&imported);
        }
#endif
#if LIGHTGW_HAS_SHT31_EXTENSION
        if (module == EXTENSION_MODULE_SHT31) {
            const sht31_extension_config_t imported = {
                .version = SHT31_EXTENSION_CONFIG_VERSION,
                .sample_period_seconds = config->sht31_sample_period_seconds,
            };
            err = sht31_extension_set_config(&imported);
        }
#endif
        if (err != ESP_OK) return err;
        bool enabled = (config->enabled_mask & (1U << module)) != 0;
        bool module_restart = false;
        if (s_started) {
            err = extension_manager_set_enabled(module, enabled, &module_restart);
        } else {
            err = persist_enabled(module, enabled);
            if (err == ESP_OK) s_enabled[module] = enabled;
        }
        if (err != ESP_OK) return err;
        *restart_required |= module_restart;
        (*applied)++;
    }
    return ESP_OK;
}
