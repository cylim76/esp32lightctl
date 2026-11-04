#include "bh1750_extension.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "bh1750_protocol.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus_manager.h"
#include "nvs.h"
#include "nvs_flash.h"

#define BH1750_STORAGE_PARTITION "sensor_nvs"
#define BH1750_CONFIG_KEY "cfg_v1"
#define BH1750_ADDRESS_LOW  0x23
#define BH1750_ADDRESS_HIGH 0x5C
#define BH1750_COMMAND_POWER_ON 0x01
#define BH1750_COMMAND_RESET 0x07
#define BH1750_COMMAND_CONTINUOUS_HIGH_RES 0x10
#define BH1750_I2C_TIMEOUT_MS 200
#define BH1750_RETRY_COUNT 3

static const char *TAG = "bh1750_ext";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static TaskHandle_t s_task;
static volatile bool s_stop_requested;
static bh1750_extension_config_t s_config;
static bh1750_extension_status_t s_status;
static size_t s_history_head;
static float s_filter_values[3];
static size_t s_filter_count;
static size_t s_filter_head;
static uint8_t s_anomaly_count;

static void default_config(bh1750_extension_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->version = BH1750_EXTENSION_CONFIG_VERSION;
    config->sample_period_seconds = 30;
    config->night_start_minute = 22 * 60;
    config->night_end_minute = 6 * 60;
    config->night_lux_threshold = 10.0f;
    config->consecutive_anomaly_limit = 3;
}

static bool config_valid(const bh1750_extension_config_t *config)
{
    return config && config->version == BH1750_EXTENSION_CONFIG_VERSION &&
           config->sample_period_seconds >= 10 && config->sample_period_seconds <= 300 &&
           config->night_start_minute < 1440 && config->night_end_minute < 1440 &&
           isfinite(config->night_lux_threshold) && config->night_lux_threshold >= 0.0f &&
           config->night_lux_threshold <= 10000.0f &&
           config->consecutive_anomaly_limit >= 1 && config->consecutive_anomaly_limit <= 20 &&
           (config->calibration_valid_mask & ~0x1FU) == 0;
}

static void set_error(esp_err_t err, const char *operation)
{
    portENTER_CRITICAL(&s_lock);
    s_status.online = false;
    s_status.data_valid = false;
    s_status.consecutive_errors++;
    snprintf(s_status.last_error, sizeof(s_status.last_error), "%s: %s",
             operation, esp_err_to_name(err));
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t load_config(void)
{
    default_config(&s_config);
    esp_err_t err = nvs_flash_init_partition(BH1750_STORAGE_PARTITION);
    if (err != ESP_OK) return err;
    nvs_handle_t nvs;
    err = nvs_open_from_partition(BH1750_STORAGE_PARTITION,
                                  BH1750_EXTENSION_CONFIG_NAMESPACE,
                                  NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    bh1750_extension_config_t stored;
    size_t length = sizeof(stored);
    err = nvs_get_blob(nvs, BH1750_CONFIG_KEY, &stored, &length);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (length != sizeof(stored) || !config_valid(&stored)) {
        ESP_LOGE(TAG, "Stored config invalid; safe defaults used without erasing NVS");
        return ESP_ERR_INVALID_CRC;
    }
    s_config = stored;
    return ESP_OK;
}

static esp_err_t save_config(const bh1750_extension_config_t *config)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open_from_partition(BH1750_STORAGE_PARTITION,
                                             BH1750_EXTENSION_CONFIG_NAMESPACE,
                                             NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, BH1750_CONFIG_KEY, config, sizeof(*config));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void release_device(void)
{
    if (s_device) {
        i2c_master_bus_rm_device(s_device);
        s_device = NULL;
    }
}

static esp_err_t send_command(uint8_t command)
{
    return s_device ? i2c_master_transmit(s_device, &command, 1, BH1750_I2C_TIMEOUT_MS) :
                      ESP_ERR_INVALID_STATE;
}

static esp_err_t attach_and_configure(void)
{
    if (s_device) return ESP_OK;
    esp_err_t err = i2c_bus_manager_get(&s_bus);
    if (err != ESP_OK) return err;
    const uint8_t addresses[] = {BH1750_ADDRESS_LOW, BH1750_ADDRESS_HIGH};
    uint8_t address = 0;
    for (size_t i = 0; i < sizeof(addresses); ++i) {
        if (i2c_master_probe(s_bus, addresses[i], BH1750_I2C_TIMEOUT_MS) == ESP_OK) {
            address = addresses[i];
            break;
        }
    }
    if (!address) return ESP_ERR_NOT_FOUND;
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(s_bus, &device_config, &s_device);
    if (err != ESP_OK) return err;
    if ((err = send_command(BH1750_COMMAND_POWER_ON)) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(2));
        err = send_command(BH1750_COMMAND_RESET);
    }
    if (err == ESP_OK) err = send_command(BH1750_COMMAND_CONTINUOUS_HIGH_RES);
    if (err != ESP_OK) {
        release_device();
        return err;
    }
    portENTER_CRITICAL(&s_lock);
    s_status.present = true;
    s_status.i2c_address = address;
    strlcpy(s_status.last_error, "none", sizeof(s_status.last_error));
    portEXIT_CRITICAL(&s_lock);
    vTaskDelay(pdMS_TO_TICKS(180));
    return ESP_OK;
}

static bool trusted_local_time(struct tm *local)
{
    time_t now = time(NULL);
    if (now <= 0 || !localtime_r(&now, local)) return false;
    int year = local->tm_year + 1900;
    return year > 2026 || (year == 2026 && (local->tm_mon > 10 ||
           (local->tm_mon == 10 && local->tm_mday >= 1)));
}

static bool minute_in_night_window(uint16_t minute, uint16_t start, uint16_t end)
{
    if (start == end) return false;
    return start < end ? minute >= start && minute < end : minute >= start || minute < end;
}

static void record_sample(float lux)
{
    s_filter_values[s_filter_head] = lux;
    s_filter_head = (s_filter_head + 1) % 3;
    if (s_filter_count < 3) s_filter_count++;
    float filtered = bh1750_protocol_median(s_filter_values, s_filter_count);
    int64_t uptime_ms = esp_timer_get_time() / 1000;
    bh1750_extension_config_t config;
    portENTER_CRITICAL(&s_lock);
    config = s_config;
    portEXIT_CRITICAL(&s_lock);
    struct tm local;
    bool night = trusted_local_time(&local) && minute_in_night_window(
        (uint16_t)(local.tm_hour * 60 + local.tm_min), config.night_start_minute,
        config.night_end_minute);

    portENTER_CRITICAL(&s_lock);
    bh1750_sample_t *sample = &s_status.history[s_history_head];
    sample->uptime_ms = uptime_ms;
    sample->lux = lux;
    sample->filtered_lux = filtered;
    s_history_head = (s_history_head + 1) % BH1750_EXTENSION_HISTORY_COUNT;
    if (s_status.history_count < BH1750_EXTENSION_HISTORY_COUNT) s_status.history_count++;
    s_status.lux = lux;
    s_status.filtered_lux = filtered;
    s_status.data_valid = true;
    s_status.present = true;
    s_status.online = true;
    s_status.successful_samples++;
    s_status.consecutive_errors = 0;
    s_status.last_success_uptime_ms = uptime_ms;
    strlcpy(s_status.last_error, "none", sizeof(s_status.last_error));
    if (night && filtered > config.night_lux_threshold) {
        if (s_anomaly_count < UINT8_MAX) s_anomaly_count++;
    } else {
        s_anomaly_count = 0;
    }
    s_status.night_anomaly = s_anomaly_count >= config.consecutive_anomaly_limit;
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t take_sample(float *lux)
{
    esp_err_t err = attach_and_configure();
    if (err != ESP_OK) return err;
    uint8_t bytes[2];
    err = i2c_master_receive(s_device, bytes, sizeof(bytes), BH1750_I2C_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    float decoded = bh1750_protocol_decode_lux(bytes[0], bytes[1]);
    if (!bh1750_protocol_lux_valid(decoded)) return ESP_ERR_INVALID_RESPONSE;
    *lux = decoded;
    return ESP_OK;
}

static void sampling_task(void *argument)
{
    (void)argument;
    while (!s_stop_requested) {
        esp_err_t err = ESP_FAIL;
        float lux = 0;
        for (uint8_t attempt = 0; attempt < BH1750_RETRY_COUNT && !s_stop_requested; ++attempt) {
            err = take_sample(&lux);
            if (err == ESP_OK) break;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (err == ESP_OK) {
            record_sample(lux);
        } else if (!s_stop_requested) {
            set_error(err, "sample");
            release_device();
        }
        uint16_t interval;
        portENTER_CRITICAL(&s_lock);
        interval = s_config.sample_period_seconds;
        portEXIT_CRITICAL(&s_lock);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((uint32_t)interval * 1000U));
    }
    release_device();
    portENTER_CRITICAL(&s_lock);
    s_status.running = false;
    s_status.online = false;
    s_status.data_valid = false;
    s_status.night_anomaly = false;
    s_status.history_count = 0;
    s_task = NULL;
    portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

esp_err_t bh1750_extension_start(void)
{
    portENTER_CRITICAL(&s_lock);
    bool running = s_status.running;
    portEXIT_CRITICAL(&s_lock);
    if (running) return ESP_OK;
    esp_err_t config_err = load_config();
    if (config_err != ESP_OK && config_err != ESP_ERR_INVALID_CRC) {
        set_error(config_err, "config load");
        return config_err;
    }
    portENTER_CRITICAL(&s_lock);
    memset(&s_status, 0, sizeof(s_status));
    strlcpy(s_status.last_error, config_err == ESP_OK ? "none" : "invalid config; safe defaults",
            sizeof(s_status.last_error));
    s_stop_requested = false;
    s_history_head = 0;
    s_filter_count = 0;
    s_filter_head = 0;
    s_anomaly_count = 0;
    portEXIT_CRITICAL(&s_lock);
    if (xTaskCreate(sampling_task, "bh1750", 3072, NULL, 4, &s_task) != pdPASS) {
        set_error(ESP_ERR_NO_MEM, "task create");
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_lock);
    s_status.running = true;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Sampling task started, interval=%us", s_config.sample_period_seconds);
    return ESP_OK;
}

esp_err_t bh1750_extension_stop(void)
{
    portENTER_CRITICAL(&s_lock);
    TaskHandle_t task = s_task;
    s_stop_requested = true;
    portEXIT_CRITICAL(&s_lock);
    if (!task) return ESP_OK;
    xTaskNotifyGive(task);
    for (uint8_t wait = 0; wait < 20; ++wait) {
        portENTER_CRITICAL(&s_lock);
        bool stopped = s_task == NULL;
        portEXIT_CRITICAL(&s_lock);
        if (stopped) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return ESP_ERR_TIMEOUT;
}

void bh1750_extension_get_status(bh1750_extension_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    if (s_status.history_count) {
        size_t oldest = (s_history_head + BH1750_EXTENSION_HISTORY_COUNT -
                         s_status.history_count) % BH1750_EXTENSION_HISTORY_COUNT;
        for (size_t i = 0; i < s_status.history_count; ++i) {
            status->history[i] = s_status.history[(oldest + i) % BH1750_EXTENSION_HISTORY_COUNT];
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

void bh1750_extension_get_config(bh1750_extension_config_t *config)
{
    if (!config) return;
    portENTER_CRITICAL(&s_lock);
    *config = s_config;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t bh1750_extension_set_config(const bh1750_extension_config_t *config)
{
    if (!config_valid(config)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    bool unchanged = memcmp(&s_config, config, sizeof(*config)) == 0;
    portEXIT_CRITICAL(&s_lock);
    if (unchanged) return ESP_OK;
    esp_err_t err = save_config(config);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_config = *config;
        portEXIT_CRITICAL(&s_lock);
        if (s_task) xTaskNotifyGive(s_task);
    }
    return err;
}

esp_err_t bh1750_extension_record_calibration(bh1750_calibration_scene_t scene)
{
    if (scene < BH1750_CAL_ALL_OFF || scene >= BH1750_EXTENSION_CALIBRATION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    bh1750_extension_config_t updated;
    portENTER_CRITICAL(&s_lock);
    bool valid = s_status.data_valid;
    updated = s_config;
    if (valid) {
        updated.calibration_lux[scene] = s_status.filtered_lux;
        updated.calibration_valid_mask |= 1U << scene;
    }
    portEXIT_CRITICAL(&s_lock);
    return valid ? bh1750_extension_set_config(&updated) : ESP_ERR_INVALID_STATE;
}
