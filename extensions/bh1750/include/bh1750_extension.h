#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define BH1750_EXTENSION_CONFIG_NAMESPACE "bh1750_ext"
#define BH1750_EXTENSION_CONFIG_VERSION   1

/* Experimental presence probe only; no task, history buffer, or NVS write. */
esp_err_t bh1750_extension_start(void);
bool bh1750_extension_is_present(void);
