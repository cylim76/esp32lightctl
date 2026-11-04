#include "rainmaker_extension.h"
#include "rainmaker_control_policy.h"

#include <inttypes.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_rmaker_common_events.h"
#include "esp_rmaker_core.h"
#include "esp_rmaker_standard_devices.h"
#include "esp_rmaker_standard_params.h"
#include "esp_rmaker_standard_types.h"
#include "esp_rmaker_user_mapping.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CLOUD_DEVICE_COUNT 7
#define REPORT_PERIOD_MS 5000
#define SENSOR_REPORT_PERIOD_MS 30000

typedef struct {
    const char *name;
    uint8_t target;
    bool action_only;
    esp_rmaker_device_t *device;
    esp_rmaker_param_t *control_param;
    esp_rmaker_param_t *status_param;
    uint32_t pending_command_id;
    int64_t last_local_update_ms;
} cloud_device_t;

static const char *TAG = "rainmaker_ext";
static rainmaker_control_api_t s_control;
static rainmaker_extension_status_t s_status = {.last_error = "not started"};
static esp_rmaker_node_t *s_node;
static TaskHandle_t s_sync_task;
static volatile bool s_task_running;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static cloud_device_t s_devices[CLOUD_DEVICE_COUNT] = {
    {.name = "Office Light Left", .target = 0},
    {.name = "Office Light Center", .target = 1},
    {.name = "Office Light Right", .target = 2},
    {.name = "All Lights OFF", .target = 3, .action_only = true},
    {.name = "LG AC 1 OFF", .target = 4, .action_only = true},
    {.name = "LG AC 2 OFF", .target = 5, .action_only = true},
    {.name = "LG AC Group OFF", .target = 6, .action_only = true},
};
static esp_rmaker_param_t *s_lux_param;
static esp_rmaker_param_t *s_lux_valid_param;
static esp_rmaker_param_t *s_temperature_param;
static esp_rmaker_param_t *s_humidity_param;
static esp_rmaker_param_t *s_climate_valid_param;

static int64_t uptime_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void set_error(const char *message)
{
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_status.last_error, message ? message : "unknown", sizeof(s_status.last_error));
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGW(TAG, "%s", message ? message : "unknown error");
}

static void set_status_param(cloud_device_t *entry, const char *message)
{
    if (entry->status_param) {
        esp_rmaker_param_update_and_report(entry->status_param, esp_rmaker_str(message));
    }
}

static esp_err_t command_write_cb(const esp_rmaker_device_t *device,
                                  const esp_rmaker_param_t *param,
                                  const esp_rmaker_param_val_t value,
                                  void *priv_data, esp_rmaker_write_ctx_t *ctx)
{
    (void)device;
    cloud_device_t *entry = priv_data;
    if (!entry || param != entry->control_param || value.type != RMAKER_VAL_TYPE_BOOLEAN) {
        return ESP_ERR_INVALID_ARG;
    }
    bool requested_on = false;
    rainmaker_policy_decision_t decision = rainmaker_control_policy_prepare(
        entry->action_only, value.val.b, ctx && ctx->src == ESP_RMAKER_REQ_SRC_INIT,
        entry->pending_command_id != 0, &requested_on);
    if (decision == RAINMAKER_POLICY_IGNORE_INIT) {
        ESP_LOGW(TAG, "Ignored cached init value for %s", entry->name);
        return ESP_OK;
    }
    if (decision == RAINMAKER_POLICY_IGNORE_RELEASE) return ESP_OK;
    if (decision == RAINMAKER_POLICY_REJECT_BUSY) return ESP_ERR_INVALID_STATE;
    uint32_t command_id = 0;
    esp_err_t err = s_control.submit_target(entry->target, requested_on,
                                             "rainmaker", &command_id);
    if (err != ESP_OK) {
        set_status_param(entry, "command rejected by local control service");
        return err;
    }
    entry->pending_command_id = command_id;
    set_status_param(entry, entry->action_only ? "OFF command queued" :
                                               "awaiting fresh Zigbee feedback");
    if (entry->action_only) {
        /* A trigger is not a physical state. Always reset it immediately. */
        esp_rmaker_param_update_and_report(entry->control_param, esp_rmaker_bool(false));
    }
    ESP_LOGI(TAG, "Cloud command queued target=%u on=%u id=%" PRIu32,
             entry->target, requested_on, command_id);
    return ESP_OK;
}

static esp_err_t add_light_device(cloud_device_t *entry)
{
    entry->device = esp_rmaker_switch_device_create(entry->name, entry, false);
    if (!entry->device) return ESP_ERR_NO_MEM;
    entry->control_param = esp_rmaker_device_get_param_by_name(entry->device,
                                                               ESP_RMAKER_DEF_POWER_NAME);
    entry->status_param = esp_rmaker_param_create("Confirmation", "esp.param.confirmation",
                                                  esp_rmaker_str("state unknown"), PROP_FLAG_READ);
    if (!entry->control_param || !entry->status_param) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_rmaker_device_add_param(entry->device, entry->status_param);
    if (err == ESP_OK) err = esp_rmaker_device_add_cb(entry->device, command_write_cb, NULL);
    if (err == ESP_OK) err = esp_rmaker_node_add_device(s_node, entry->device);
    return err;
}

static esp_err_t add_off_action_device(cloud_device_t *entry)
{
    entry->device = esp_rmaker_device_create(entry->name, ESP_RMAKER_DEVICE_OTHER, entry);
    if (!entry->device) return ESP_ERR_NO_MEM;
    esp_rmaker_param_t *name = esp_rmaker_name_param_create(ESP_RMAKER_DEF_NAME_PARAM, entry->name);
    entry->control_param = esp_rmaker_param_create("Send OFF", "esp.param.explicit-off",
                                                   esp_rmaker_bool(false),
                                                   PROP_FLAG_READ | PROP_FLAG_WRITE);
    entry->status_param = esp_rmaker_param_create("Result", "esp.param.off-result",
                                                  esp_rmaker_str("not sent"), PROP_FLAG_READ);
    if (!name || !entry->control_param || !entry->status_param) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_rmaker_param_add_ui_type(entry->control_param, ESP_RMAKER_UI_TRIGGER);
    if (err == ESP_OK) err = esp_rmaker_device_add_param(entry->device, name);
    if (err == ESP_OK) err = esp_rmaker_device_add_param(entry->device, entry->control_param);
    if (err == ESP_OK) err = esp_rmaker_device_add_param(entry->device, entry->status_param);
    if (err == ESP_OK) err = esp_rmaker_device_assign_primary_param(entry->device,
                                                                    entry->control_param);
    if (err == ESP_OK) err = esp_rmaker_device_add_cb(entry->device, command_write_cb, NULL);
    if (err == ESP_OK) err = esp_rmaker_node_add_device(s_node, entry->device);
    return err;
}

static esp_err_t add_environment_device(void)
{
    esp_rmaker_device_t *device = esp_rmaker_device_create("Office Environment",
                                                           ESP_RMAKER_DEVICE_OTHER, NULL);
    if (!device) return ESP_ERR_NO_MEM;
    esp_rmaker_param_t *name = esp_rmaker_name_param_create(ESP_RMAKER_DEF_NAME_PARAM,
                                                            "Office Environment");
    s_lux_param = esp_rmaker_param_create("Lux", "esp.param.lux", esp_rmaker_float(0),
                                          PROP_FLAG_READ | PROP_FLAG_TIME_SERIES);
    s_lux_valid_param = esp_rmaker_param_create("Lux Valid", "esp.param.lux-valid",
                                                esp_rmaker_bool(false), PROP_FLAG_READ);
    s_temperature_param = esp_rmaker_temperature_param_create(ESP_RMAKER_DEF_TEMPERATURE_NAME, 0);
    s_humidity_param = esp_rmaker_param_create("Humidity", "esp.param.humidity",
                                               esp_rmaker_float(0),
                                               PROP_FLAG_READ | PROP_FLAG_TIME_SERIES);
    s_climate_valid_param = esp_rmaker_param_create("Climate Valid", "esp.param.climate-valid",
                                                    esp_rmaker_bool(false), PROP_FLAG_READ);
    if (!name || !s_lux_param || !s_lux_valid_param || !s_temperature_param ||
        !s_humidity_param || !s_climate_valid_param) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_rmaker_device_add_param(device, name);
    if (err == ESP_OK) err = esp_rmaker_device_add_param(device, s_lux_param);
    if (err == ESP_OK) err = esp_rmaker_device_add_param(device, s_lux_valid_param);
    if (err == ESP_OK) err = esp_rmaker_device_add_param(device, s_temperature_param);
    if (err == ESP_OK) err = esp_rmaker_device_add_param(device, s_humidity_param);
    if (err == ESP_OK) err = esp_rmaker_device_add_param(device, s_climate_valid_param);
    if (err == ESP_OK) err = esp_rmaker_node_add_device(s_node, device);
    return err;
}

static void process_pending_command(cloud_device_t *entry)
{
    if (!entry->pending_command_id) return;
    rainmaker_command_result_t result = {0};
    if (s_control.get_command_result(entry->pending_command_id, &result) != ESP_OK ||
        result.state == RAINMAKER_COMMAND_PENDING) return;

    if (result.state == RAINMAKER_COMMAND_CONFIRMED) {
        if (!entry->action_only) {
            esp_rmaker_param_update_and_report(entry->control_param,
                                               esp_rmaker_bool(result.requested_on));
            entry->last_local_update_ms = uptime_ms();
        }
        set_status_param(entry, "confirmed by fresh Zigbee endpoint feedback");
    } else if (result.state == RAINMAKER_COMMAND_SENT_UNCONFIRMED && entry->action_only) {
        set_status_param(entry, "IR sent; air-conditioner physical state unknown");
    } else {
        set_status_param(entry, result.detail[0] ? result.detail : "command failed");
    }
    entry->pending_command_id = 0;
}

static void sync_light_state(cloud_device_t *entry)
{
    if (entry->pending_command_id || entry->action_only) return;
    rainmaker_light_state_t local = {0};
    if (s_control.get_light_state(entry->target, &local) != ESP_OK || !local.valid ||
        local.updated_uptime_ms <= entry->last_local_update_ms) return;
    esp_rmaker_param_update_and_report(entry->control_param, esp_rmaker_bool(local.on));
    set_status_param(entry, "fresh local endpoint state");
    entry->last_local_update_ms = local.updated_uptime_ms;
}

static void report_environment(void)
{
    rainmaker_environment_t environment = {0};
    s_control.get_environment(&environment);
    esp_rmaker_param_update(s_lux_valid_param, esp_rmaker_bool(environment.lux_valid));
    if (environment.lux_valid) esp_rmaker_param_update(s_lux_param, esp_rmaker_float(environment.lux));
    esp_rmaker_param_update(s_climate_valid_param, esp_rmaker_bool(environment.climate_valid));
    if (environment.climate_valid) {
        esp_rmaker_param_update(s_temperature_param, esp_rmaker_float(environment.temperature_c));
        esp_rmaker_param_update(s_humidity_param, esp_rmaker_float(environment.humidity_percent));
    }
    esp_rmaker_report_updated_params();
}

static void sync_task(void *arg)
{
    (void)arg;
    int64_t last_report_ms = 0;
    while (s_task_running) {
        for (size_t i = 0; i < CLOUD_DEVICE_COUNT; ++i) {
            process_pending_command(&s_devices[i]);
            sync_light_state(&s_devices[i]);
        }
        int64_t now = uptime_ms();
        if (s_control.environment_available && now - last_report_ms >= SENSOR_REPORT_PERIOD_MS) {
            report_environment();
            last_report_ms = now;
        }
        esp_rmaker_user_mapping_state_t mapping = esp_rmaker_user_node_mapping_get_state();
        portENTER_CRITICAL(&s_lock);
        s_status.claimed = mapping == ESP_RMAKER_USER_MAPPING_DONE;
        s_status.mapping_in_progress = mapping == ESP_RMAKER_USER_MAPPING_STARTED ||
                                       mapping == ESP_RMAKER_USER_MAPPING_REQ_SENT;
        portEXIT_CRITICAL(&s_lock);
        vTaskDelay(pdMS_TO_TICKS(REPORT_PERIOD_MS));
    }
    s_sync_task = NULL;
    vTaskDelete(NULL);
}

static void event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg;
    (void)data;
    if (base == RMAKER_COMMON_EVENT) {
        if (event_id == RMAKER_MQTT_EVENT_CONNECTED) {
            portENTER_CRITICAL(&s_lock);
            s_status.online = true;
            s_status.last_success_uptime_ms = uptime_ms();
            strlcpy(s_status.last_error, "none", sizeof(s_status.last_error));
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "RainMaker MQTT connected");
        } else if (event_id == RMAKER_MQTT_EVENT_DISCONNECTED) {
            portENTER_CRITICAL(&s_lock);
            s_status.online = false;
            portEXIT_CRITICAL(&s_lock);
            set_error("RainMaker MQTT disconnected; Core remains available");
        }
    } else if (base == RMAKER_EVENT) {
        if (event_id == RMAKER_EVENT_CLAIM_FAILED) {
            set_error("RainMaker self-claim failed");
        } else if (event_id == RMAKER_EVENT_USER_NODE_MAPPING_DONE) {
            portENTER_CRITICAL(&s_lock);
            s_status.claimed = true;
            s_status.mapping_in_progress = false;
            s_status.last_success_uptime_ms = uptime_ms();
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "RainMaker user-node mapping request completed");
        }
    }
}

esp_err_t rainmaker_extension_start(const rainmaker_control_api_t *control_api)
{
    if (!control_api || !control_api->submit_target || !control_api->get_command_result ||
        !control_api->get_light_state || !control_api->get_environment) return ESP_ERR_INVALID_ARG;
    if (s_status.running) return ESP_OK;
    if (s_node) return ESP_ERR_INVALID_STATE;
    s_control = *control_api;
    esp_rmaker_config_t config = {.enable_time_sync = false};
    s_node = esp_rmaker_node_init(&config, "esp32lightctl Office Gateway",
                                  "esp32lightctl.gateway");
    if (!s_node) {
        set_error("esp_rmaker_node_init failed; check claim credentials and NVS");
        return ESP_FAIL;
    }
    esp_err_t err = esp_rmaker_node_add_model(s_node, "ESP32-C6 Light Gateway");
    for (size_t i = 0; err == ESP_OK && i < CLOUD_DEVICE_COUNT; ++i) {
        err = s_devices[i].action_only ? add_off_action_device(&s_devices[i]) :
                                         add_light_device(&s_devices[i]);
    }
    if (err == ESP_OK && s_control.environment_available) err = add_environment_device();
    if (err != ESP_OK) {
        set_error("RainMaker device graph creation failed");
        esp_rmaker_node_deinit(s_node);
        s_node = NULL;
        return err;
    }
    esp_event_handler_register(RMAKER_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL);
    esp_event_handler_register(RMAKER_COMMON_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL);
    err = esp_rmaker_start();
    if (err != ESP_OK) {
        set_error("esp_rmaker_start failed");
        return err;
    }
    char *node_id = esp_rmaker_get_node_id();
    portENTER_CRITICAL(&s_lock);
    s_status.running = true;
    s_status.online = false;
    s_status.claimed = esp_rmaker_user_node_mapping_get_state() == ESP_RMAKER_USER_MAPPING_DONE;
    s_status.mapping_in_progress = false;
    strlcpy(s_status.node_id, node_id ? node_id : "unavailable", sizeof(s_status.node_id));
    strlcpy(s_status.last_error, "none", sizeof(s_status.last_error));
    portEXIT_CRITICAL(&s_lock);
    s_task_running = true;
    if (xTaskCreate(sync_task, "rainmaker_sync", 6144, NULL, 3, &s_sync_task) != pdPASS) {
        s_task_running = false;
        set_error("RainMaker sync task allocation failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "RainMaker Classic started node_id=%s", s_status.node_id);
    return ESP_OK;
}

esp_err_t rainmaker_extension_stop(void)
{
    if (!s_status.running) return ESP_OK;
    /* v1.17 only marks STOP_REQUESTED and exposes no safe completion event for
     * node deinit/reinit. Persist disable, then require a controlled reboot. */
    portENTER_CRITICAL(&s_lock);
    s_status.online = false;
    strlcpy(s_status.last_error, "disable saved; controlled reboot required",
            sizeof(s_status.last_error));
    portEXIT_CRITICAL(&s_lock);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t rainmaker_extension_start_user_mapping(char *user_id, char *secret_key)
{
    if (!user_id || !secret_key || !user_id[0] || !secret_key[0]) return ESP_ERR_INVALID_ARG;
    if (!s_status.running || !s_status.online) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_rmaker_start_user_node_mapping(user_id, secret_key);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_status.mapping_in_progress = true;
        portEXIT_CRITICAL(&s_lock);
    } else {
        set_error("user-node mapping request rejected");
    }
    return err;
}

void rainmaker_extension_get_status(rainmaker_extension_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}
