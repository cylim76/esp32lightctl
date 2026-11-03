#pragma once

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

typedef struct {
    bool initialized;
    bool online;
    bool osf;
    uint8_t status_register;
    esp_err_t last_error;
} rtc_manager_status_t;

esp_err_t rtc_manager_init(void);
esp_err_t rtc_manager_read_utc(time_t *utc, bool *osf);
esp_err_t rtc_manager_write_utc(time_t utc);
void rtc_manager_get_status(rtc_manager_status_t *status);
