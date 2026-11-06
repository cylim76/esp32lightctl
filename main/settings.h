#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define APP_MAX_SCHEDULES_PER_TARGET 8
#define APP_SCHEDULE_TARGET_COUNT 7
#define APP_MAX_SCHEDULES (APP_MAX_SCHEDULES_PER_TARGET * APP_SCHEDULE_TARGET_COUNT)
#define APP_MAX_SCHEDULE_RETRIES 3

typedef enum {
    SCHEDULE_TARGET_LIGHT_LEFT = 0,
    SCHEDULE_TARGET_LIGHT_CENTER,
    SCHEDULE_TARGET_LIGHT_RIGHT,
    SCHEDULE_TARGET_LIGHT_ALL,
    SCHEDULE_TARGET_AC_1,
    SCHEDULE_TARGET_AC_2,
    SCHEDULE_TARGET_AC_GROUP,
} schedule_target_t;

typedef struct {
    uint8_t enabled;
    uint8_t target;
    uint8_t hour;
    uint8_t minute;
    uint8_t days;
    uint8_t retry_count;
    uint8_t retry_offsets_minutes[APP_MAX_SCHEDULE_RETRIES];
    uint16_t catchup_window_minutes;
} app_schedule_entry_t;

typedef struct {
    bool wifi_configured;
    char wifi_ssid[33];
    char wifi_password[65];
    char device_name[33];
    char admin_user[33];
    uint8_t auth_salt[16];
    uint8_t auth_hash[32];
    uint32_t auth_iterations;
    bool auth_configured;
    uint8_t schedule_count;
    app_schedule_entry_t schedules[APP_MAX_SCHEDULES];
    char timezone[65];
} app_settings_t;

esp_err_t settings_init(void);
const app_settings_t *settings_get(void);
void settings_copy(app_settings_t *out);
esp_err_t settings_save_wifi(const char *ssid, const char *password);
esp_err_t settings_clear_wifi(void);
esp_err_t settings_save_device_name(const char *device_name);
esp_err_t settings_save_admin(const char *user, const char *password);
esp_err_t settings_save_admin_verifier(const char *user, const uint8_t *salt, size_t salt_len,
                                       const uint8_t *hash, size_t hash_len,
                                       uint32_t iterations);
bool settings_verify_admin(const char *user, const char *password);
esp_err_t settings_upgrade_admin_hash_async(const char *user, const char *password);
esp_err_t settings_save_schedules(const app_schedule_entry_t *entries, size_t count,
                                  const char *timezone);
const char *settings_schedule_target_name(schedule_target_t target);
