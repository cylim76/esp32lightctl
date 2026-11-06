#include "settings.h"

#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "auth_password.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "storage_manager.h"

static const char *TAG = "settings";
static app_settings_t s_cfg;
static SemaphoreHandle_t s_cfg_lock;
static bool s_force_schedule_write;
static bool s_auth_migration_pending;

static esp_err_t open_settings_rw(nvs_handle_t *nvs, const char *operation)
{
    esp_err_t err = nvs_open(APP_NVS_NAMESPACE, NVS_READWRITE, nvs);
    if (err != ESP_OK) storage_manager_record_write("nvs", operation, err, true);
    return err;
}

const char *settings_schedule_target_name(schedule_target_t target)
{
    static const char *names[] = {"LIGHT_LEFT", "LIGHT_CENTER", "LIGHT_RIGHT", "LIGHT_ALL",
                                  "AC_1", "AC_2", "AC_GROUP"};
    return target < APP_SCHEDULE_TARGET_COUNT ? names[target] : "INVALID";
}

static bool valid_device_name(const char *name)
{
    if (!name) return false;
    size_t len = strlen(name);
    if (len < 1 || len > 32 || name[0] == '-' || name[len - 1] == '-') return false;
    for (size_t i = 0; i < len; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

static esp_err_t derive_password_iterations(const char *password, const uint8_t *salt,
                                            uint32_t iterations, uint8_t *out)
{
    /* ESP32-C6 is single-core. Yielding during the legacy 120k derivation
       prevents the HTTP task from starving IDLE and firing TWDT. */
    return auth_password_pbkdf2_sha256(password, salt, APP_AUTH_SALT_LEN,
                                       iterations, 64, out);
}

static esp_err_t derive_password(const char *password, const uint8_t *salt, uint8_t *out)
{
    return derive_password_iterations(password, salt, APP_AUTH_ITERATIONS, out);
}

static bool constant_time_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) diff |= a[i] ^ b[i];
    return diff == 0;
}

static void load_str(nvs_handle_t nvs, const char *key, char *out, size_t cap)
{
    size_t len = cap;
    esp_err_t err = nvs_get_str(nvs, key, out, &len);
    if (err != ESP_OK) {
        out[0] = '\0';
        if (err != ESP_ERR_NVS_NOT_FOUND) storage_manager_record_corruption("nvs", key);
    }
}

static bool valid_schedule_list(const app_schedule_entry_t *entries, size_t count)
{
    if ((!entries && count) || count > APP_MAX_SCHEDULES) return false;
    uint8_t per_target[APP_SCHEDULE_TARGET_COUNT] = {0};
    for (size_t i = 0; i < count; ++i) {
        const app_schedule_entry_t *entry = &entries[i];
        if (entry->enabled > 1 || entry->target >= APP_SCHEDULE_TARGET_COUNT ||
            entry->hour > 23 || entry->minute > 59 || (entry->days & 0x80U) ||
            entry->retry_count > APP_MAX_SCHEDULE_RETRIES || entry->catchup_window_minutes > 180 ||
            ++per_target[entry->target] > APP_MAX_SCHEDULES_PER_TARGET) return false;
        uint8_t previous = 0;
        for (size_t retry = 0; retry < entry->retry_count; ++retry) {
            uint8_t offset = entry->retry_offsets_minutes[retry];
            if (!offset || offset <= previous || offset > entry->catchup_window_minutes) return false;
            previous = offset;
        }
    }
    return true;
}

esp_err_t settings_init(void)
{
    if (!s_cfg_lock) s_cfg_lock = xSemaphoreCreateMutex();
    if (!s_cfg_lock) return ESP_ERR_NO_MEM;
    memset(&s_cfg, 0, sizeof(s_cfg));
    strlcpy(s_cfg.admin_user, "admin", sizeof(s_cfg.admin_user));
    strlcpy(s_cfg.device_name, "lightgw", sizeof(s_cfg.device_name));
    strlcpy(s_cfg.timezone, APP_TIMEZONE_DEFAULT, sizeof(s_cfg.timezone));
    s_cfg.auth_iterations = APP_AUTH_ITERATIONS;

    if (!storage_manager_primary_ready()) {
        ESP_LOGE(TAG, "Primary NVS unavailable; using volatile defaults without erasing stored data");
        return ESP_OK;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(APP_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    load_str(nvs, "wifi_ssid", s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid));
    load_str(nvs, "wifi_pass", s_cfg.wifi_password, sizeof(s_cfg.wifi_password));
    load_str(nvs, "device_name", s_cfg.device_name, sizeof(s_cfg.device_name));
    if (!valid_device_name(s_cfg.device_name)) strlcpy(s_cfg.device_name, "lightgw", sizeof(s_cfg.device_name));
    load_str(nvs, "admin_user", s_cfg.admin_user, sizeof(s_cfg.admin_user));
    load_str(nvs, "timezone", s_cfg.timezone, sizeof(s_cfg.timezone));
    if (strcmp(s_cfg.timezone, APP_TIMEZONE_DEFAULT) != 0) {
        ESP_LOGW(TAG, "Unsupported saved timezone '%s'; forcing Beijing time %s", s_cfg.timezone, APP_TIMEZONE_DEFAULT);
        strlcpy(s_cfg.timezone, APP_TIMEZONE_DEFAULT, sizeof(s_cfg.timezone));
    }
    s_cfg.wifi_configured = s_cfg.wifi_ssid[0] != '\0';

    size_t len = sizeof(s_cfg.auth_salt);
    if (nvs_get_blob(nvs, "auth_salt", s_cfg.auth_salt, &len) == ESP_OK && len == sizeof(s_cfg.auth_salt)) {
        len = sizeof(s_cfg.auth_hash);
        s_cfg.auth_configured = nvs_get_blob(nvs, "auth_hash", s_cfg.auth_hash, &len) == ESP_OK && len == sizeof(s_cfg.auth_hash);
        uint32_t saved_iterations = 0;
        esp_err_t iteration_err = nvs_get_u32(nvs, "auth_iter", &saved_iterations);
        if (iteration_err == ESP_OK &&
            saved_iterations >= 1000 && saved_iterations <= 1000000) {
            s_cfg.auth_iterations = saved_iterations;
        } else if (iteration_err == ESP_ERR_NVS_NOT_FOUND && s_cfg.auth_configured) {
            /* Firmware before auth_iter always used 120000 iterations. */
            s_cfg.auth_iterations = APP_AUTH_LEGACY_ITERATIONS;
        }
    }
    bool migrated_schedules = false;
    uint8_t schedule_version = 0;
    bool has_v3_schedules = nvs_get_u8(nvs, "sched_ver", &schedule_version) == ESP_OK && schedule_version == 3;
    if (has_v3_schedules) {
        size_t schedules_len = sizeof(s_cfg.schedules);
        if (nvs_get_blob(nvs, "sched_list", s_cfg.schedules, &schedules_len) == ESP_OK &&
            schedules_len % sizeof(app_schedule_entry_t) == 0) {
            s_cfg.schedule_count = schedules_len / sizeof(app_schedule_entry_t);
            if (s_cfg.schedule_count > APP_MAX_SCHEDULES) s_cfg.schedule_count = APP_MAX_SCHEDULES;
            if (!valid_schedule_list(s_cfg.schedules, s_cfg.schedule_count)) {
                ESP_LOGW(TAG, "Ignoring invalid saved schedule list");
                storage_manager_record_corruption("nvs", "sched_list validation failed");
                memset(s_cfg.schedules, 0, sizeof(s_cfg.schedules));
                s_cfg.schedule_count = 0;
            }
        }
    } else if (schedule_version == 2) {
        typedef struct { uint8_t enabled, hour, minute, days; } legacy_v2_t;
        legacy_v2_t legacy[APP_MAX_SCHEDULES] = {0};
        size_t legacy_len = sizeof(legacy);
        if (nvs_get_blob(nvs, "sched_list", legacy, &legacy_len) == ESP_OK &&
            legacy_len % sizeof(legacy_v2_t) == 0) {
            s_cfg.schedule_count = legacy_len / sizeof(legacy_v2_t);
            if (s_cfg.schedule_count > APP_MAX_SCHEDULES) s_cfg.schedule_count = APP_MAX_SCHEDULES;
            for (size_t i = 0; i < s_cfg.schedule_count; ++i) {
                s_cfg.schedules[i] = (app_schedule_entry_t){
                    .enabled = legacy[i].enabled, .target = SCHEDULE_TARGET_LIGHT_ALL,
                    .hour = legacy[i].hour, .minute = legacy[i].minute,
                    .days = legacy[i].days, .retry_count = 3,
                    .retry_offsets_minutes = {1, 5, 10}, .catchup_window_minutes = 30,
                };
            }
            migrated_schedules = true;
            ESP_LOGI(TAG, "Migrated %u schedule(s) from v2 to v3 global-light targets", (unsigned)s_cfg.schedule_count);
        }
    } else {
        /* One-time compatibility path for firmware versions that stored one schedule. */
        uint8_t enabled = 0, hour = 0, minute = 0, days = 0;
        bool legacy_present = nvs_get_u8(nvs, "sched_en", &enabled) == ESP_OK;
        nvs_get_u8(nvs, "sched_hour", &hour);
        nvs_get_u8(nvs, "sched_min", &minute);
        nvs_get_u8(nvs, "sched_days", &days);
        if (legacy_present && hour <= 23 && minute <= 59) {
            s_cfg.schedule_count = 1;
            s_cfg.schedules[0] = (app_schedule_entry_t){
                .enabled = enabled != 0, .target = SCHEDULE_TARGET_LIGHT_ALL,
                .hour = hour, .minute = minute, .days = days,
                .retry_count = 3, .retry_offsets_minutes = {1, 5, 10},
                .catchup_window_minutes = 30,
            };
            migrated_schedules = true;
        }
    }
    nvs_close(nvs);
    if (migrated_schedules) {
        s_force_schedule_write = true;
        err = settings_save_schedules(s_cfg.schedules, s_cfg.schedule_count, APP_TIMEZONE_DEFAULT);
        s_force_schedule_write = false;
        if (err != ESP_OK) ESP_LOGE(TAG, "Persisting migrated schedules failed: %s", esp_err_to_name(err));
        /* Keep the valid legacy data in RAM and retry migration next boot; never erase it. */
        return ESP_OK;
    }
    return ESP_OK;
}

esp_err_t settings_clear_wifi(void)
{
    if (!s_cfg.wifi_configured && !s_cfg.wifi_ssid[0] && !s_cfg.wifi_password[0]) {
        storage_manager_record_write("nvs", "wifi", ESP_OK, false);
        return ESP_OK;
    }
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(open_settings_rw(&nvs, "wifi_clear_open"), TAG, "open NVS");
    esp_err_t err = nvs_erase_key(nvs, "wifi_ssid");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    esp_err_t pass_err = nvs_erase_key(nvs, "wifi_pass");
    if (pass_err == ESP_ERR_NVS_NOT_FOUND) pass_err = ESP_OK;
    if (err == ESP_OK) err = pass_err;
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    storage_manager_record_write("nvs", "wifi", err, true);
    if (err == ESP_OK) {
        memset(s_cfg.wifi_ssid, 0, sizeof(s_cfg.wifi_ssid));
        memset(s_cfg.wifi_password, 0, sizeof(s_cfg.wifi_password));
        s_cfg.wifi_configured = false;
    }
    return err;
}

esp_err_t settings_save_device_name(const char *device_name)
{
    if (!valid_device_name(device_name)) return ESP_ERR_INVALID_ARG;
    if (strcmp(s_cfg.device_name, device_name) == 0) {
        storage_manager_record_write("nvs", "device_name", ESP_OK, false);
        return ESP_OK;
    }
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(open_settings_rw(&nvs, "device_name_open"), TAG, "open NVS");
    esp_err_t err = nvs_set_str(nvs, "device_name", device_name);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    storage_manager_record_write("nvs", "device_name", err, true);
    if (err == ESP_OK) strlcpy(s_cfg.device_name, device_name, sizeof(s_cfg.device_name));
    return err;
}

const app_settings_t *settings_get(void) { return &s_cfg; }

void settings_copy(app_settings_t *out)
{
    if (!out) return;
    if (s_cfg_lock) xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    *out = s_cfg;
    if (s_cfg_lock) xSemaphoreGive(s_cfg_lock);
}

esp_err_t settings_save_wifi(const char *ssid, const char *password)
{
    if (!ssid || !password || ssid[0] == '\0' || strlen(ssid) > 32 || strlen(password) > 64) return ESP_ERR_INVALID_ARG;
    if (s_cfg.wifi_configured && strcmp(s_cfg.wifi_ssid, ssid) == 0 &&
        strcmp(s_cfg.wifi_password, password) == 0) {
        storage_manager_record_write("nvs", "wifi", ESP_OK, false);
        return ESP_OK;
    }
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(open_settings_rw(&nvs, "wifi_open"), TAG, "open NVS");
    esp_err_t err = nvs_set_str(nvs, "wifi_ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "wifi_pass", password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    storage_manager_record_write("nvs", "wifi", err, true);
    if (err == ESP_OK) {
        strlcpy(s_cfg.wifi_ssid, ssid, sizeof(s_cfg.wifi_ssid));
        strlcpy(s_cfg.wifi_password, password, sizeof(s_cfg.wifi_password));
        s_cfg.wifi_configured = true;
    }
    return err;
}

esp_err_t settings_save_admin(const char *user, const char *password)
{
    if (!user || !password || strlen(user) < 1 || strlen(user) > 32 || strlen(password) < 12 || strlen(password) > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cfg.auth_configured && strcmp(s_cfg.admin_user, user) == 0 &&
        settings_verify_admin(user, password)) {
        storage_manager_record_write("nvs", "admin", ESP_OK, false);
        return ESP_OK;
    }
    uint8_t salt[APP_AUTH_SALT_LEN], hash[APP_AUTH_HASH_LEN];
    esp_fill_random(salt, sizeof(salt));
    ESP_RETURN_ON_ERROR(derive_password(password, salt, hash), TAG, "password derivation");
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(open_settings_rw(&nvs, "admin_open"), TAG, "open NVS");
    esp_err_t err = nvs_set_str(nvs, "admin_user", user);
    if (err == ESP_OK) err = nvs_set_blob(nvs, "auth_salt", salt, sizeof(salt));
    if (err == ESP_OK) err = nvs_set_blob(nvs, "auth_hash", hash, sizeof(hash));
    if (err == ESP_OK) err = nvs_set_u32(nvs, "auth_iter", APP_AUTH_ITERATIONS);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    storage_manager_record_write("nvs", "admin", err, true);
    if (err == ESP_OK) {
        strlcpy(s_cfg.admin_user, user, sizeof(s_cfg.admin_user));
        memcpy(s_cfg.auth_salt, salt, sizeof(salt));
        memcpy(s_cfg.auth_hash, hash, sizeof(hash));
        s_cfg.auth_iterations = APP_AUTH_ITERATIONS;
        s_cfg.auth_configured = true;
    }
    return err;
}

esp_err_t settings_save_admin_verifier(const char *user, const uint8_t *salt, size_t salt_len,
                                       const uint8_t *hash, size_t hash_len,
                                       uint32_t iterations)
{
    if (!user || strlen(user) < 1 || strlen(user) > 32 ||
        !salt || salt_len != APP_AUTH_SALT_LEN || !hash || hash_len != APP_AUTH_HASH_LEN ||
        iterations != APP_AUTH_ITERATIONS) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(open_settings_rw(&nvs, "admin_verifier_open"), TAG, "open NVS");
    esp_err_t err = nvs_set_str(nvs, "admin_user", user);
    if (err == ESP_OK) err = nvs_set_blob(nvs, "auth_salt", salt, salt_len);
    if (err == ESP_OK) err = nvs_set_blob(nvs, "auth_hash", hash, hash_len);
    if (err == ESP_OK) err = nvs_set_u32(nvs, "auth_iter", iterations);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    storage_manager_record_write("nvs", "admin_verifier", err, true);
    if (err == ESP_OK) {
        xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
        strlcpy(s_cfg.admin_user, user, sizeof(s_cfg.admin_user));
        memcpy(s_cfg.auth_salt, salt, sizeof(s_cfg.auth_salt));
        memcpy(s_cfg.auth_hash, hash, sizeof(s_cfg.auth_hash));
        s_cfg.auth_iterations = iterations;
        s_cfg.auth_configured = true;
        xSemaphoreGive(s_cfg_lock);
    }
    return err;
}

bool settings_verify_admin(const char *user, const char *password)
{
    if (!s_cfg.auth_configured || !user || !password || strcmp(user, s_cfg.admin_user) != 0) return false;
    uint8_t hash[APP_AUTH_HASH_LEN];
    if (derive_password_iterations(password, s_cfg.auth_salt, s_cfg.auth_iterations, hash) != ESP_OK) return false;
    bool accepted = constant_time_equal(hash, s_cfg.auth_hash, sizeof(hash));
    memset(hash, 0, sizeof(hash));
    return accepted;
}

typedef struct {
    char user[33];
    char password[65];
} auth_migration_args_t;

static void auth_migration_task(void *argument)
{
    auth_migration_args_t *args = argument;
    uint8_t salt[APP_AUTH_SALT_LEN];
    uint8_t hash[APP_AUTH_HASH_LEN];
    esp_fill_random(salt, sizeof(salt));
    esp_err_t err = derive_password(args->password, salt, hash);
    if (err == ESP_OK) {
        err = settings_save_admin_verifier(args->user, salt, sizeof(salt), hash, sizeof(hash),
                                           APP_AUTH_ITERATIONS);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Administrator hash migrated from legacy %u to %u iterations",
                 APP_AUTH_LEGACY_ITERATIONS, APP_AUTH_ITERATIONS);
    } else {
        ESP_LOGE(TAG, "Administrator hash migration failed: %s; legacy verifier retained",
                 esp_err_to_name(err));
    }
    memset(hash, 0, sizeof(hash));
    memset(salt, 0, sizeof(salt));
    memset(args, 0, sizeof(*args));
    free(args);
    s_auth_migration_pending = false;
    vTaskDelete(NULL);
}

esp_err_t settings_upgrade_admin_hash_async(const char *user, const char *password)
{
    if (!user || !password || !s_cfg.auth_configured ||
        strcmp(user, s_cfg.admin_user) != 0 || s_cfg.auth_iterations == APP_AUTH_ITERATIONS) {
        return ESP_OK;
    }
    if (s_auth_migration_pending) return ESP_OK;
    auth_migration_args_t *args = calloc(1, sizeof(*args));
    if (!args) return ESP_ERR_NO_MEM;
    strlcpy(args->user, user, sizeof(args->user));
    strlcpy(args->password, password, sizeof(args->password));
    s_auth_migration_pending = true;
    if (xTaskCreate(auth_migration_task, "auth_migrate", 4096, args, 2, NULL) != pdPASS) {
        s_auth_migration_pending = false;
        memset(args, 0, sizeof(*args));
        free(args);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t settings_save_schedules(const app_schedule_entry_t *entries, size_t count, const char *timezone)
{
    if ((!entries && count != 0) || count > APP_MAX_SCHEDULES || !timezone ||
        strcmp(timezone, APP_TIMEZONE_DEFAULT) != 0) return ESP_ERR_INVALID_ARG;
    if (!valid_schedule_list(entries, count)) return ESP_ERR_INVALID_ARG;
    app_schedule_entry_t snapshot[APP_MAX_SCHEDULES] = {0};
    if (count) memcpy(snapshot, entries, count * sizeof(*entries));
    bool unchanged = !s_force_schedule_write && s_cfg.schedule_count == count && strcmp(s_cfg.timezone, timezone) == 0 &&
                     (!count || memcmp(s_cfg.schedules, entries, count * sizeof(*entries)) == 0);
    if (unchanged) {
        storage_manager_record_write("nvs", "schedules", ESP_OK, false);
        return ESP_OK;
    }
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(open_settings_rw(&nvs, "schedules_open"), TAG, "open NVS");
    esp_err_t err = count ? nvs_set_blob(nvs, "sched_list", snapshot, count * sizeof(*snapshot)) :
                            nvs_erase_key(nvs, "sched_list");
    if (!count && err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_set_u8(nvs, "sched_ver", 3);
    if (err == ESP_OK) err = nvs_set_str(nvs, "timezone", timezone);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    storage_manager_record_write("nvs", "schedules", err, true);
    if (err == ESP_OK) {
        xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
        memset(s_cfg.schedules, 0, sizeof(s_cfg.schedules));
        if (count) memcpy(s_cfg.schedules, snapshot, count * sizeof(*snapshot));
        s_cfg.schedule_count = count;
        strlcpy(s_cfg.timezone, timezone, sizeof(s_cfg.timezone));
        xSemaphoreGive(s_cfg_lock);
    }
    return err;
}
