#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

/* Core-owned singleton bus. Optional devices may attach, but never own/delete it. */
esp_err_t i2c_bus_manager_init(void);
esp_err_t i2c_bus_manager_get(i2c_master_bus_handle_t *bus);
