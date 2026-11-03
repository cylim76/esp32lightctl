#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool wifi_configured;
    char wifi_ssid[33];
    char wifi_password[65];
    char admin_user[33];
    uint8_t auth_salt[16];
    uint8_t auth_hash[32];
    bool auth_configured;
    bool schedule_enabled;
    uint8_t schedule_hour;
    uint8_t schedule_minute;
    uint8_t schedule_days;
    char timezone[65];
} app_settings_t;

esp_err_t settings_init(void);
const app_settings_t *settings_get(void);
esp_err_t settings_save_wifi(const char *ssid, const char *password);
esp_err_t settings_save_admin(const char *user, const char *password);
bool settings_verify_admin(const char *user, const char *password);
esp_err_t settings_save_schedule(bool enabled, uint8_t hour, uint8_t minute,
                                 uint8_t days, const char *timezone);
