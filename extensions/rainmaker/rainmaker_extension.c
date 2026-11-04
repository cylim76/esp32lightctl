#include "rainmaker_extension.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "rainmaker_ext";
static rainmaker_extension_status_t s_status = {.last_error = "not started"};

esp_err_t rainmaker_extension_start(const rainmaker_control_api_t *control_api)
{
    if (!control_api || !control_api->submit_target) return ESP_ERR_INVALID_ARG;
    s_status.running = false;
    s_status.online = false;
    strlcpy(s_status.last_error, "transport/provisioning not implemented",
            sizeof(s_status.last_error));
    ESP_LOGW(TAG, "RainMaker adapter boundary enabled, but transport/provisioning is not implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t rainmaker_extension_stop(void)
{
    s_status.running = false;
    s_status.online = false;
    strlcpy(s_status.last_error, "disabled; no transport was active", sizeof(s_status.last_error));
    return ESP_OK;
}

void rainmaker_extension_get_status(rainmaker_extension_status_t *status)
{
    if (status) *status = s_status;
}
