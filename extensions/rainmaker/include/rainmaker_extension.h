#pragma once

#include <stdint.h>
#include "esp_err.h"

#define RAINMAKER_EXTENSION_CONFIG_NAMESPACE "rainmaker_ext"
#define RAINMAKER_EXTENSION_CONFIG_VERSION   1

typedef esp_err_t (*rainmaker_submit_target_fn)(uint8_t target, const char *source,
                                                uint32_t *command_id);

typedef struct {
    rainmaker_submit_target_fn submit_target;
} rainmaker_control_api_t;

/* Adapter boundary only. The experimental transport will live in this component. */
esp_err_t rainmaker_extension_start(const rainmaker_control_api_t *control_api);
