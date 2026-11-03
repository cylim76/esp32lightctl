#pragma once

#include "esp_err.h"

/* Optional extension failures are diagnosed but never fail Core startup. */
esp_err_t extension_manager_start(void);
