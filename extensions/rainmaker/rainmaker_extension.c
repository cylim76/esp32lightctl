#include "rainmaker_extension.h"

#include "esp_log.h"

static const char *TAG = "rainmaker_ext";

esp_err_t rainmaker_extension_start(const rainmaker_control_api_t *control_api)
{
    if (!control_api || !control_api->submit_target) return ESP_ERR_INVALID_ARG;
    ESP_LOGW(TAG, "RainMaker adapter boundary enabled, but transport/provisioning is not implemented");
    return ESP_ERR_NOT_SUPPORTED;
}
