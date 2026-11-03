#include "settings.h"

#include <string.h>
#include "app_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "nvs.h"

static const char *TAG = "settings";
static app_settings_t s_cfg;

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

static esp_err_t derive_password(const char *password, const uint8_t *salt, uint8_t *out)
{
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
        (const unsigned char *)password, strlen(password), salt, APP_AUTH_SALT_LEN,
        APP_AUTH_ITERATIONS, APP_AUTH_HASH_LEN, out) == 0 ? ESP_OK : ESP_FAIL;
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
    if (nvs_get_str(nvs, key, out, &len) != ESP_OK) out[0] = '\0';
}

esp_err_t settings_init(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    strlcpy(s_cfg.admin_user, "admin", sizeof(s_cfg.admin_user));
    strlcpy(s_cfg.device_name, "lightgw", sizeof(s_cfg.device_name));
    strlcpy(s_cfg.timezone, APP_TIMEZONE_DEFAULT, sizeof(s_cfg.timezone));

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
    s_cfg.wifi_configured = s_cfg.wifi_ssid[0] != '\0';

    size_t len = sizeof(s_cfg.auth_salt);
    if (nvs_get_blob(nvs, "auth_salt", s_cfg.auth_salt, &len) == ESP_OK && len == sizeof(s_cfg.auth_salt)) {
        len = sizeof(s_cfg.auth_hash);
        s_cfg.auth_configured = nvs_get_blob(nvs, "auth_hash", s_cfg.auth_hash, &len) == ESP_OK && len == sizeof(s_cfg.auth_hash);
    }
    uint8_t schedule_version = 0;
    bool has_v2_schedules = nvs_get_u8(nvs, "sched_ver", &schedule_version) == ESP_OK && schedule_version == 2;
    if (has_v2_schedules) {
        size_t schedules_len = sizeof(s_cfg.schedules);
        if (nvs_get_blob(nvs, "sched_list", s_cfg.schedules, &schedules_len) == ESP_OK &&
            schedules_len % sizeof(app_schedule_entry_t) == 0) {
            s_cfg.schedule_count = schedules_len / sizeof(app_schedule_entry_t);
            if (s_cfg.schedule_count > APP_MAX_SCHEDULES) s_cfg.schedule_count = APP_MAX_SCHEDULES;
            for (size_t i = 0; i < s_cfg.schedule_count; ++i) {
                app_schedule_entry_t *entry = &s_cfg.schedules[i];
                if (entry->enabled > 1 || entry->hour > 23 || entry->minute > 59 || (entry->days & 0x80U)) {
                    ESP_LOGW(TAG, "Ignoring invalid saved schedule list");
                    memset(s_cfg.schedules, 0, sizeof(s_cfg.schedules));
                    s_cfg.schedule_count = 0;
                    break;
                }
            }
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
            s_cfg.schedules[0] = (app_schedule_entry_t){enabled != 0, hour, minute, days};
        }
    }
    nvs_close(nvs);
    return ESP_OK;
}

esp_err_t settings_clear_wifi(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(APP_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open NVS");
    esp_err_t err = nvs_erase_key(nvs, "wifi_ssid");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    esp_err_t pass_err = nvs_erase_key(nvs, "wifi_pass");
    if (pass_err == ESP_ERR_NVS_NOT_FOUND) pass_err = ESP_OK;
    if (err == ESP_OK) err = pass_err;
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
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
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(APP_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open NVS");
    esp_err_t err = nvs_set_str(nvs, "device_name", device_name);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) strlcpy(s_cfg.device_name, device_name, sizeof(s_cfg.device_name));
    return err;
}

const app_settings_t *settings_get(void) { return &s_cfg; }

esp_err_t settings_save_wifi(const char *ssid, const char *password)
{
    if (!ssid || !password || ssid[0] == '\0' || strlen(ssid) > 32 || strlen(password) > 64) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(APP_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open NVS");
    esp_err_t err = nvs_set_str(nvs, "wifi_ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "wifi_pass", password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
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
    uint8_t salt[APP_AUTH_SALT_LEN], hash[APP_AUTH_HASH_LEN];
    esp_fill_random(salt, sizeof(salt));
    ESP_RETURN_ON_ERROR(derive_password(password, salt, hash), TAG, "password derivation");
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(APP_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open NVS");
    esp_err_t err = nvs_set_str(nvs, "admin_user", user);
    if (err == ESP_OK) err = nvs_set_blob(nvs, "auth_salt", salt, sizeof(salt));
    if (err == ESP_OK) err = nvs_set_blob(nvs, "auth_hash", hash, sizeof(hash));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) {
        strlcpy(s_cfg.admin_user, user, sizeof(s_cfg.admin_user));
        memcpy(s_cfg.auth_salt, salt, sizeof(salt));
        memcpy(s_cfg.auth_hash, hash, sizeof(hash));
        s_cfg.auth_configured = true;
    }
    return err;
}

bool settings_verify_admin(const char *user, const char *password)
{
    if (!s_cfg.auth_configured || !user || !password || strcmp(user, s_cfg.admin_user) != 0) return false;
    uint8_t hash[APP_AUTH_HASH_LEN];
    if (derive_password(password, s_cfg.auth_salt, hash) != ESP_OK) return false;
    return constant_time_equal(hash, s_cfg.auth_hash, sizeof(hash));
}

esp_err_t settings_save_schedules(const app_schedule_entry_t *entries, size_t count, const char *timezone)
{
    if ((!entries && count != 0) || count > APP_MAX_SCHEDULES || !timezone ||
        timezone[0] == '\0' || strlen(timezone) > 64) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].enabled > 1 || entries[i].hour > 23 || entries[i].minute > 59 ||
            (entries[i].days & 0x80U) != 0) return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(APP_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open NVS");
    esp_err_t err = count ? nvs_set_blob(nvs, "sched_list", entries, count * sizeof(*entries)) :
                            nvs_erase_key(nvs, "sched_list");
    if (!count && err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_set_u8(nvs, "sched_ver", 2);
    if (err == ESP_OK) err = nvs_set_str(nvs, "timezone", timezone);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) {
        memset(s_cfg.schedules, 0, sizeof(s_cfg.schedules));
        if (count) memcpy(s_cfg.schedules, entries, count * sizeof(*entries));
        s_cfg.schedule_count = count;
        strlcpy(s_cfg.timezone, timezone, sizeof(s_cfg.timezone));
    }
    return err;
}
