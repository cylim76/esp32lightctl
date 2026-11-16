#pragma once

#include "esp_err.h"

/* Prepare the ESP Zigbee 2.x NVS destination and migrate legacy 1.x data.
 * This function never erases either partition. It must run before
 * esp_zigbee_init(). */
esp_err_t zigbee_nvram_prepare(void);
