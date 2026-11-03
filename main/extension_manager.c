#include "extension_manager.h"

#include "diagnostics.h"
#include "esp_log.h"
#include "stage_config.h"

#if LIGHTGW_HAS_BH1750_EXTENSION
#include "bh1750_extension.h"
#endif
#if LIGHTGW_HAS_SHT31_EXTENSION
#include "sht31_extension.h"
#endif
#if LIGHTGW_HAS_RAINMAKER_EXTENSION
#include "device_control.h"
#include "rainmaker_extension.h"
#endif

static const char *TAG = "extensions";

#if LIGHTGW_HAS_RAINMAKER_EXTENSION
static esp_err_t cloud_submit_target(uint8_t target, const char *source, uint32_t *command_id)
{
    if (target >= APP_SCHEDULE_TARGET_COUNT) return ESP_ERR_INVALID_ARG;
    return device_control_submit_target((schedule_target_t)target, source, command_id);
}
#endif

esp_err_t extension_manager_start(void)
{
#if LIGHTGW_HAS_BH1750_EXTENSION
    esp_err_t bh1750_err = bh1750_extension_start();
    if (bh1750_err != ESP_OK) {
        diagnostics_set_error(TAG, "BH1750 extension unavailable: %s; Core continues",
                              esp_err_to_name(bh1750_err));
        ESP_LOGW(TAG, "BH1750 extension unavailable: %s", esp_err_to_name(bh1750_err));
    }
#endif
#if LIGHTGW_HAS_SHT31_EXTENSION
    esp_err_t sht31_err = sht31_extension_start();
    if (sht31_err != ESP_OK) {
        diagnostics_set_error(TAG, "SHT31 extension unavailable: %s; Core continues",
                              esp_err_to_name(sht31_err));
        ESP_LOGW(TAG, "SHT31 extension unavailable: %s", esp_err_to_name(sht31_err));
    }
#endif
#if LIGHTGW_HAS_RAINMAKER_EXTENSION
    const rainmaker_control_api_t api = {.submit_target = cloud_submit_target};
    esp_err_t cloud_err = rainmaker_extension_start(&api);
    if (cloud_err != ESP_OK) {
        diagnostics_set_error(TAG, "RainMaker extension unavailable: %s; Core continues",
                              esp_err_to_name(cloud_err));
        ESP_LOGW(TAG, "RainMaker extension unavailable: %s", esp_err_to_name(cloud_err));
    }
#endif
    return ESP_OK;
}
