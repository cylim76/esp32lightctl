#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define SHT31_EXTENSION_CONFIG_NAMESPACE "sht31_ext"
#define SHT31_EXTENSION_CONFIG_VERSION   1

/* Experimental presence probe only; no task, history buffer, or NVS write. */
esp_err_t sht31_extension_start(void);
bool sht31_extension_is_present(void);
