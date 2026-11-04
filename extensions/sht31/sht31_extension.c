#include "sht31_extension.h"

#include <stdio.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus_manager.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sht31_protocol.h"

#define SHT31_STORAGE_PARTITION "sensor_nvs"
#define SHT31_CONFIG_KEY "cfg_v1"
#define SHT31_ADDRESS_LOW  0x44
#define SHT31_ADDRESS_HIGH 0x45
#define SHT31_I2C_TIMEOUT_MS 200
#define SHT31_RETRY_COUNT 3
#define SHT31_COMMAND_MEASURE_HIGH_REPEATABILITY 0x2400U

static const char *TAG = "sht31_ext";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static TaskHandle_t s_task;
static volatile bool s_stop_requested;
static sht31_extension_config_t s_config;
static sht31_extension_status_t s_status;
static size_t s_history_head;

static void default_config(sht31_extension_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->version = SHT31_EXTENSION_CONFIG_VERSION;
    config->sample_period_seconds = 30;
}

static bool config_valid(const sht31_extension_config_t *config)
{
    return config && config->version == SHT31_EXTENSION_CONFIG_VERSION &&
           config->sample_period_seconds >= 30 && config->sample_period_seconds <= 60;
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

static esp_err_t load_config(sht31_extension_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    default_config(config);
    esp_err_t err = nvs_flash_init_partition(SHT31_STORAGE_PARTITION);
    if (err != ESP_OK) return err;
    nvs_handle_t nvs;
    err = nvs_open_from_partition(SHT31_STORAGE_PARTITION,
                                  SHT31_EXTENSION_CONFIG_NAMESPACE,
                                  NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    sht31_extension_config_t stored;
    size_t length = sizeof(stored);
    err = nvs_get_blob(nvs, SHT31_CONFIG_KEY, &stored, &length);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (length != sizeof(stored) || !config_valid(&stored)) {
        ESP_LOGE(TAG, "Stored config invalid; safe defaults used without erasing NVS");
        return ESP_ERR_INVALID_CRC;
    }
    *config = stored;
    return ESP_OK;
}

static esp_err_t save_config(const sht31_extension_config_t *config)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open_from_partition(SHT31_STORAGE_PARTITION,
                                             SHT31_EXTENSION_CONFIG_NAMESPACE,
                                             NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, SHT31_CONFIG_KEY, config, sizeof(*config));
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

static esp_err_t attach_and_reset(void)
{
    if (s_device) return ESP_OK;
    esp_err_t err = i2c_bus_manager_get(&s_bus);
    if (err != ESP_OK) return err;
    const uint8_t addresses[] = {SHT31_ADDRESS_LOW, SHT31_ADDRESS_HIGH};
    uint8_t address = 0;
    for (size_t i = 0; i < sizeof(addresses); ++i) {
        if (i2c_master_probe(s_bus, addresses[i], SHT31_I2C_TIMEOUT_MS) == ESP_OK) {
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
    const uint8_t reset_command[] = {0x30, 0xA2};
    err = i2c_master_transmit(s_device, reset_command, sizeof(reset_command),
                              SHT31_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        release_device();
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    portENTER_CRITICAL(&s_lock);
    s_status.present = true;
    s_status.i2c_address = address;
    strlcpy(s_status.last_error, "none", sizeof(s_status.last_error));
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

static esp_err_t take_sample(float *temperature_c, float *humidity_percent)
{
    esp_err_t err = attach_and_reset();
    if (err != ESP_OK) return err;
    const uint8_t command[] = {
        (uint8_t)(SHT31_COMMAND_MEASURE_HIGH_REPEATABILITY >> 8),
        (uint8_t)SHT31_COMMAND_MEASURE_HIGH_REPEATABILITY,
    };
    err = i2c_master_transmit(s_device, command, sizeof(command), SHT31_I2C_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t frame[6];
    err = i2c_master_receive(s_device, frame, sizeof(frame), SHT31_I2C_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    return sht31_protocol_decode(frame, temperature_c, humidity_percent) ? ESP_OK :
           ESP_ERR_INVALID_CRC;
}

static sht31_temperature_trend_t calculate_trend(float current)
{
    if (s_status.history_count < 2) return SHT31_TREND_UNKNOWN;
    size_t oldest = (s_history_head + SHT31_EXTENSION_HISTORY_COUNT -
                     s_status.history_count) % SHT31_EXTENSION_HISTORY_COUNT;
    float difference = current - s_status.history[oldest].temperature_c;
    if (difference > 0.3f) return SHT31_TREND_RISING;
    if (difference < -0.3f) return SHT31_TREND_FALLING;
    return SHT31_TREND_STABLE;
}

static void record_sample(float temperature_c, float humidity_percent)
{
    int64_t uptime_ms = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_lock);
    sht31_temperature_trend_t trend = calculate_trend(temperature_c);
    sht31_sample_t *sample = &s_status.history[s_history_head];
    sample->uptime_ms = uptime_ms;
    sample->temperature_c = temperature_c;
    sample->humidity_percent = humidity_percent;
    s_history_head = (s_history_head + 1) % SHT31_EXTENSION_HISTORY_COUNT;
    if (s_status.history_count < SHT31_EXTENSION_HISTORY_COUNT) s_status.history_count++;
    s_status.temperature_c = temperature_c;
    s_status.humidity_percent = humidity_percent;
    s_status.temperature_trend = trend;
    s_status.data_valid = true;
    s_status.present = true;
    s_status.online = true;
    s_status.successful_samples++;
    s_status.consecutive_errors = 0;
    s_status.last_success_uptime_ms = uptime_ms;
    strlcpy(s_status.last_error, "none", sizeof(s_status.last_error));
    portEXIT_CRITICAL(&s_lock);
}

static void sampling_task(void *argument)
{
    (void)argument;
    while (!s_stop_requested) {
        esp_err_t err = ESP_FAIL;
        float temperature_c = 0;
        float humidity_percent = 0;
        for (uint8_t attempt = 0; attempt < SHT31_RETRY_COUNT && !s_stop_requested; ++attempt) {
            err = take_sample(&temperature_c, &humidity_percent);
            if (err == ESP_OK) break;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (err == ESP_OK) {
            record_sample(temperature_c, humidity_percent);
        } else if (!s_stop_requested) {
            set_error(err, "sample/CRC");
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
    s_status.temperature_trend = SHT31_TREND_UNKNOWN;
    s_status.history_count = 0;
    s_task = NULL;
    portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

esp_err_t sht31_extension_start(void)
{
    portENTER_CRITICAL(&s_lock);
    bool running = s_status.running;
    portEXIT_CRITICAL(&s_lock);
    if (running) return ESP_OK;
    sht31_extension_config_t config;
    esp_err_t config_err = load_config(&config);
    if (config_err != ESP_OK && config_err != ESP_ERR_INVALID_CRC) {
        set_error(config_err, "config load");
        return config_err;
    }
    portENTER_CRITICAL(&s_lock);
    s_config = config;
    memset(&s_status, 0, sizeof(s_status));
    strlcpy(s_status.last_error, config_err == ESP_OK ? "none" : "invalid config; safe defaults",
            sizeof(s_status.last_error));
    s_stop_requested = false;
    s_history_head = 0;
    portEXIT_CRITICAL(&s_lock);
    if (xTaskCreate(sampling_task, "sht31", 3072, NULL, 4, &s_task) != pdPASS) {
        set_error(ESP_ERR_NO_MEM, "task create");
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_lock);
    s_status.running = true;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Sampling task started, interval=%us", s_config.sample_period_seconds);
    return ESP_OK;
}

esp_err_t sht31_extension_stop(void)
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

void sht31_extension_get_status(sht31_extension_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    if (s_status.history_count) {
        size_t oldest = (s_history_head + SHT31_EXTENSION_HISTORY_COUNT -
                         s_status.history_count) % SHT31_EXTENSION_HISTORY_COUNT;
        for (size_t i = 0; i < s_status.history_count; ++i) {
            status->history[i] = s_status.history[(oldest + i) % SHT31_EXTENSION_HISTORY_COUNT];
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

void sht31_extension_get_config(sht31_extension_config_t *config)
{
    if (!config) return;
    portENTER_CRITICAL(&s_lock);
    *config = s_config;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t sht31_extension_read_config(sht31_extension_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    bool loaded = config_valid(&s_config);
    if (loaded) *config = s_config;
    portEXIT_CRITICAL(&s_lock);
    if (loaded) return ESP_OK;
    return load_config(config);
}

esp_err_t sht31_extension_set_config(const sht31_extension_config_t *config)
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
