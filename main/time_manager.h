#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

typedef enum {
    TIME_SOURCE_INVALID = 0,
    TIME_SOURCE_SYSTEM,
    TIME_SOURCE_RTC,
    TIME_SOURCE_MANUAL,
    TIME_SOURCE_NTP,
} time_source_t;

typedef struct {
    bool valid;
    time_source_t source;
    bool rtc_initialized;
    bool rtc_online;
    bool rtc_osf;
    uint8_t rtc_status_register;
    time_t now_utc;
    time_t last_ntp_sync;
    time_t next_ntp_sync;
    bool ntp_in_progress;
    char last_ntp_result[96];
    char last_error[96];
    char timezone[65];
} time_manager_status_t;

esp_err_t time_manager_start(void);
bool time_manager_is_valid(void);
const char *time_manager_source_name(time_source_t source);
void time_manager_get_status(time_manager_status_t *status);
esp_err_t time_manager_request_ntp(void);
esp_err_t time_manager_set_local(int year, int month, int day, int hour, int minute, int second);
bool time_manager_epoch_valid(time_t value);
