#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define RAINMAKER_EXTENSION_CONFIG_NAMESPACE "rainmaker_ext"
#define RAINMAKER_EXTENSION_CONFIG_VERSION   1

typedef esp_err_t (*rainmaker_submit_target_fn)(uint8_t target, const char *source,
                                                uint32_t *command_id);

typedef struct {
    rainmaker_submit_target_fn submit_target;
} rainmaker_control_api_t;

typedef struct {
    bool running;
    bool online;
    int64_t last_success_uptime_ms;
    char last_error[96];
} rainmaker_extension_status_t;

/* Adapter boundary only. The experimental transport will live in this component. */
esp_err_t rainmaker_extension_start(const rainmaker_control_api_t *control_api);
esp_err_t rainmaker_extension_stop(void);
void rainmaker_extension_get_status(rainmaker_extension_status_t *status);
