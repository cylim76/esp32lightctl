#include "web_server.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "app_config.h"
#include "backup_restore.h"
#include "diagnostics.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "extension_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include "storage_manager.h"
#include "stage_config.h"
#if LIGHTGW_HAS_CONTROL
#include "device_control.h"
#endif
#include "time_manager.h"
#include "wifi_manager.h"
#if LIGHTGW_HAS_SCHEDULER
#include "scheduler.h"
#endif
#if LIGHTGW_HAS_IR
#include "ir_manager.h"
#endif
#if LIGHTGW_HAS_ZIGBEE
#include "zigbee_manager.h"
#endif

static const char *TAG = "web";
static httpd_handle_t s_server;
static int64_t s_last_backup_upload_us;
#define AUTH_TOKEN_BYTES 32
#define AUTH_SESSION_TTL_US (12LL * 60LL * 60LL * 1000000LL)
static uint8_t s_auth_session[AUTH_TOKEN_BYTES];
static bool s_auth_session_valid;
static int64_t s_auth_session_expires_us;
static int64_t s_last_auth_attempt_us;
extern const unsigned char index_html_start[] asm("_binary_index_html_start");
extern const unsigned char index_html_end[] asm("_binary_index_html_end");

static esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddFalseToObject(root, "ok");
    cJSON_AddStringToObject(root, "error", message);
    char *text = cJSON_PrintUnformatted(root);
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    cJSON_Delete(root);
    return err;
}

#if LIGHTGW_HAS_CONTROL
static esp_err_t send_command_accepted(httpd_req_t *req, uint32_t command_id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddNumberToObject(root, "command_id", command_id);
    cJSON_AddStringToObject(root, "status", "PENDING");
    char *text = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    cJSON_Delete(root);
    return err;
}
#endif

static bool constant_time_equal(const uint8_t *a, const uint8_t *b, size_t length)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < length; ++i) difference |= a[i] ^ b[i];
    return difference == 0;
}

static void hex_encode(const uint8_t *input, size_t length, char *output)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        output[i * 2] = digits[input[i] >> 4];
        output[i * 2 + 1] = digits[input[i] & 0x0f];
    }
    output[length * 2] = '\0';
}

static int hex_nibble(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool hex_decode_fixed(const char *input, uint8_t *output, size_t output_length)
{
    if (!input || strlen(input) != output_length * 2) return false;
    for (size_t i = 0; i < output_length; ++i) {
        int high = hex_nibble(input[i * 2]);
        int low = hex_nibble(input[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool authorized(httpd_req_t *req)
{
    if (wifi_manager_is_provisioning() && !settings_get()->auth_configured) return true;
    int64_t now = esp_timer_get_time();
    size_t length = httpd_req_get_hdr_value_len(req, "Cookie");
    if (!s_auth_session_valid || now >= s_auth_session_expires_us || length < 80 || length > 512) goto denied;
    char *cookie = malloc(length + 1);
    if (!cookie) goto denied;
    bool valid = false;
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, length + 1) == ESP_OK) {
        const char *name = "lightgw_session=";
        char *token = strstr(cookie, name);
        if (token) {
            token += strlen(name);
            uint8_t candidate[AUTH_TOKEN_BYTES];
            char encoded[AUTH_TOKEN_BYTES * 2 + 1];
            size_t token_length = strcspn(token, "; ");
            if (token_length == AUTH_TOKEN_BYTES * 2) {
                memcpy(encoded, token, token_length);
                encoded[token_length] = '\0';
                valid = hex_decode_fixed(encoded, candidate, sizeof(candidate)) &&
                        constant_time_equal(candidate, s_auth_session, sizeof(candidate));
            }
        }
    }
    free(cookie);
    if (valid) {
        s_auth_session_expires_us = now + AUTH_SESSION_TTL_US;
        return true;
    }
denied:
    send_json_error(req, "401 Unauthorized", "Administrator login required");
    return false;
}

static cJSON *receive_json(httpd_req_t *req)
{
    /* Seven targets with eight schedules each can legitimately exceed 2 KiB. */
    if (req->content_len <= 0 || req->content_len > 16384) return NULL;
    char *body = calloc(1, req->content_len + 1);
    if (!body) return NULL;
    int total = 0;
    while (total < req->content_len) {
        int n = httpd_req_recv(req, body + total, req->content_len - total);
        if (n <= 0) { free(body); return NULL; }
        total += n;
    }
    cJSON *json = cJSON_Parse(body);
    free(body);
    return json;
}

static char *receive_body(httpd_req_t *req, size_t maximum, size_t *length_out)
{
    if (!length_out || req->content_len <= 0 || (size_t)req->content_len > maximum) return NULL;
    char *body = calloc(1, req->content_len + 1);
    if (!body) return NULL;
    int total = 0;
    while (total < req->content_len) {
        int received = httpd_req_recv(req, body + total, req->content_len - total);
        if (received <= 0) {
            free(body);
            return NULL;
        }
        total += received;
    }
    *length_out = total;
    return body;
}

static esp_err_t auth_info_get(httpd_req_t *req)
{
    app_settings_t config;
    settings_copy(&config);
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddBoolToObject(root, "configured", config.auth_configured);
    cJSON_AddStringToObject(root, "username", config.auth_configured ? config.admin_user : "admin");
    cJSON_AddBoolToObject(root, "legacy_upgrade_required",
                          config.auth_configured && config.auth_iterations != APP_AUTH_ITERATIONS);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

static esp_err_t auth_login_post(httpd_req_t *req)
{
    int64_t now = esp_timer_get_time();
    if (s_last_auth_attempt_us && now - s_last_auth_attempt_us < 1000000LL) {
        return send_json_error(req, "429 Too Many Requests", "Wait before another login attempt");
    }
    s_last_auth_attempt_us = now;
    cJSON *json = receive_json(req);
    cJSON *user = json ? cJSON_GetObjectItem(json, "username") : NULL;
    cJSON *password = json ? cJSON_GetObjectItem(json, "password") : NULL;
    char user_copy[33] = {0};
    char password_copy[65] = {0};
    bool request_valid = cJSON_IsString(user) && cJSON_IsString(password) &&
                         strlen(user->valuestring) <= 32 && strlen(password->valuestring) <= 64;
    if (request_valid) {
        strlcpy(user_copy, user->valuestring, sizeof(user_copy));
        strlcpy(password_copy, password->valuestring, sizeof(password_copy));
        memset(password->valuestring, 0, strlen(password->valuestring));
    }
    cJSON_Delete(json);
    bool accepted = request_valid && settings_verify_admin(user_copy, password_copy);
    if (!accepted) {
        memset(password_copy, 0, sizeof(password_copy));
        memset(user_copy, 0, sizeof(user_copy));
        return send_json_error(req, "401 Unauthorized", "Invalid administrator credentials");
    }

    esp_fill_random(s_auth_session, sizeof(s_auth_session));
    s_auth_session_valid = true;
    s_auth_session_expires_us = esp_timer_get_time() + AUTH_SESSION_TTL_US;
    char token[AUTH_TOKEN_BYTES * 2 + 1];
    char cookie[160];
    hex_encode(s_auth_session, sizeof(s_auth_session), token);
    snprintf(cookie, sizeof(cookie),
             "lightgw_session=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=43200", token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/json");
    ESP_LOGI(TAG, "Administrator session established; subsequent requests use an in-RAM token");
    esp_err_t response = httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Authenticated\"}");
    esp_err_t upgrade = settings_upgrade_admin_hash_async(user_copy, password_copy);
    if (upgrade != ESP_OK) ESP_LOGW(TAG, "Could not schedule legacy administrator hash upgrade: %s",
                                    esp_err_to_name(upgrade));
    memset(password_copy, 0, sizeof(password_copy));
    memset(user_copy, 0, sizeof(user_copy));
    return response;
}

static bool backup_authorized(httpd_req_t *req)
{
    if (!settings_get()->auth_configured) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "Administrator account must be configured first");
        return false;
    }
    return authorized(req);
}

static esp_err_t send_backup_summary(httpd_req_t *req, const backup_import_summary_t *summary,
                                     bool committed)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddBoolToObject(root, "committed", committed);
    if (!committed) cJSON_AddStringToObject(root, "token", summary->token);
    cJSON_AddNumberToObject(root, "schedules", summary->schedules);
    cJSON_AddNumberToObject(root, "ir_codes", summary->ir_codes);
    cJSON_AddNumberToObject(root, "extension_sections", summary->extension_sections);
    cJSON_AddNumberToObject(root, "restored_extensions", summary->restored_extensions);
    cJSON_AddNumberToObject(root, "skipped_sections", summary->skipped_sections);
    cJSON_AddBoolToObject(root, "ir_section_available", summary->ir_section_available);
    cJSON_AddBoolToObject(root, "requires_zigbee_repair", summary->requires_zigbee_repair);
    cJSON_AddStringToObject(root, "message", summary->message);
    char *text = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = text ? httpd_resp_sendstr(req, text) : ESP_ERR_NO_MEM;
    free(text);
    cJSON_Delete(root);
    return err;
}

static esp_err_t backup_export_get(httpd_req_t *req)
{
    if (!backup_authorized(req)) return ESP_OK;
    char *json = NULL;
    size_t length = 0;
    char filename[80];
    esp_err_t err = backup_restore_export_json(&json, &length, filename, sizeof(filename));
    if (err != ESP_OK) return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    char disposition[112];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_send(req, json, length);
    free(json);
    return err;
}

static esp_err_t backup_validate_post(httpd_req_t *req)
{
    if (!backup_authorized(req)) return ESP_OK;
    int64_t now = esp_timer_get_time();
    if (s_last_backup_upload_us &&
        now - s_last_backup_upload_us < (int64_t)APP_BACKUP_UPLOAD_MIN_INTERVAL_MS * 1000) {
        return send_json_error(req, "429 Too Many Requests", "Wait 10 seconds before another backup upload");
    }
    s_last_backup_upload_us = now;
    if (req->content_len <= 0 || req->content_len > BACKUP_MAX_FILE_BYTES) {
        return send_json_error(req, "413 Payload Too Large", "Backup must be 1..49152 bytes");
    }
    size_t length = 0;
    char *body = receive_body(req, BACKUP_MAX_FILE_BYTES, &length);
    if (!body) return send_json_error(req, "400 Bad Request", "Backup upload was incomplete");
    backup_import_summary_t summary = {0};
    esp_err_t err = backup_restore_validate_import(body, length, &summary);
    free(body);
    if (err == ESP_ERR_INVALID_CRC) return send_json_error(req, "422 Unprocessable Entity", "Backup integrity check failed");
    if (err == ESP_ERR_NOT_SUPPORTED) return send_json_error(req, "409 Conflict", "Backup format or firmware schema is incompatible");
    if (err != ESP_OK) return send_json_error(req, "400 Bad Request", "Backup JSON or data ranges are invalid");
    return send_backup_summary(req, &summary, false);
}

static esp_err_t backup_commit_post(httpd_req_t *req)
{
    if (!backup_authorized(req)) return ESP_OK;
    cJSON *json = receive_json(req);
    cJSON *token = json ? cJSON_GetObjectItem(json, "token") : NULL;
    cJSON *confirm = json ? cJSON_GetObjectItem(json, "confirm") : NULL;
    if (!cJSON_IsString(token) || strlen(token->valuestring) != BACKUP_IMPORT_TOKEN_LENGTH ||
        !cJSON_IsString(confirm) || strcmp(confirm->valuestring, "RESTORE_CONFIG") != 0) {
        cJSON_Delete(json);
        return send_json_error(req, "400 Bad Request", "Validated token and RESTORE_CONFIG confirmation required");
    }
    char saved_token[BACKUP_IMPORT_TOKEN_LENGTH + 1];
    strlcpy(saved_token, token->valuestring, sizeof(saved_token));
    cJSON_Delete(json);
    backup_import_summary_t summary = {0};
    esp_err_t err = backup_restore_commit_import(saved_token, &summary);
    if (err == ESP_ERR_INVALID_STATE) return send_json_error(req, "409 Conflict", "Validation expired, token does not match, or IR learning/transmission is busy");
    if (err != ESP_OK) return send_json_error(req, "500 Internal Server Error", "Import failed; original configuration retained or rollback journal kept");
    return send_backup_summary(req, &summary, true);
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
}

static esp_err_t status_get(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    diagnostics_heap_t heap;
    diagnostics_get_heap(&heap);
    cJSON *root = cJSON_CreateObject();
    char ip[16], mac[18];
    wifi_manager_get_ip(ip, sizeof(ip));
    wifi_manager_get_mac(mac, sizeof(mac));
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddStringToObject(root, "firmware", LIGHTGW_VERSION);
    cJSON_AddNumberToObject(root, "stage", LIGHTGW_STAGE);
    cJSON_AddBoolToObject(root, "provisioning", wifi_manager_is_provisioning());
    cJSON_AddStringToObject(root, "provision_ssid", wifi_manager_provision_ssid());
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_manager_is_connected());
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "mac", mac);
    cJSON_AddNumberToObject(root, "wifi_rssi", wifi_manager_get_rssi());
    cJSON_AddStringToObject(root, "device_name", settings_get()->device_name);
    cJSON_AddStringToObject(root, "mdns_hostname", wifi_manager_mdns_hostname());
    cJSON_AddBoolToObject(root, "mdns_active", wifi_manager_mdns_active());
    cJSON_AddNumberToObject(root, "uptime_seconds", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(root, "free_heap", heap.free_heap);
    cJSON_AddNumberToObject(root, "minimum_free_heap", heap.minimum_free_heap);
    cJSON_AddNumberToObject(root, "largest_free_block", heap.largest_free_block);
    cJSON_AddStringToObject(root, "reset_reason", diagnostics_reset_reason());
    char last_error[160];
    diagnostics_get_last_error(last_error, sizeof(last_error));
    cJSON_AddStringToObject(root, "last_error", last_error);
    time_manager_status_t time_status;
    time_manager_get_status(&time_status);
    cJSON_AddBoolToObject(root, "time_synced", time_status.valid);
    cJSON *time_json = cJSON_AddObjectToObject(root, "time");
    cJSON_AddBoolToObject(time_json, "valid", time_status.valid);
    cJSON_AddStringToObject(time_json, "source", time_manager_source_name(time_status.source));
    cJSON_AddStringToObject(time_json, "timezone", time_status.timezone);
    cJSON_AddNumberToObject(time_json, "now_utc_epoch", time_status.now_utc);
    char local_text[32] = "INVALID", utc_text[32] = "INVALID";
    if (time_status.valid) {
        struct tm value;
        localtime_r(&time_status.now_utc, &value);
        strftime(local_text, sizeof(local_text), "%Y-%m-%d %H:%M:%S", &value);
        gmtime_r(&time_status.now_utc, &value);
        strftime(utc_text, sizeof(utc_text), "%Y-%m-%d %H:%M:%S", &value);
    }
    cJSON_AddStringToObject(time_json, "local", local_text);
    cJSON_AddStringToObject(time_json, "utc", utc_text);
    cJSON_AddBoolToObject(time_json, "rtc_initialized", time_status.rtc_initialized);
    cJSON_AddBoolToObject(time_json, "rtc_online", time_status.rtc_online);
    cJSON_AddBoolToObject(time_json, "rtc_osf", time_status.rtc_osf);
    cJSON_AddNumberToObject(time_json, "rtc_status_register", time_status.rtc_status_register);
    cJSON_AddNumberToObject(time_json, "last_ntp_sync", time_status.last_ntp_sync);
    cJSON_AddNumberToObject(time_json, "next_ntp_sync", time_status.next_ntp_sync);
    cJSON_AddBoolToObject(time_json, "ntp_in_progress", time_status.ntp_in_progress);
    cJSON_AddStringToObject(time_json, "last_ntp_result", time_status.last_ntp_result);
    cJSON_AddStringToObject(time_json, "last_error", time_status.last_error);
#if LIGHTGW_HAS_ZIGBEE
    cJSON_AddBoolToObject(root, "zigbee_ready", zigbee_manager_network_ready());
    cJSON_AddNumberToObject(root, "zigbee_channel", zigbee_manager_channel());
    cJSON_AddNumberToObject(root, "pan_id", zigbee_manager_pan_id());
    cJSON_AddNumberToObject(root, "pairing_remaining", zigbee_manager_pairing_remaining());
#else
    cJSON_AddFalseToObject(root, "zigbee_ready");
    cJSON_AddNumberToObject(root, "zigbee_channel", 0);
    cJSON_AddNumberToObject(root, "pan_id", 65535);
    cJSON_AddNumberToObject(root, "pairing_remaining", 0);
#endif

    app_settings_t cfg_snapshot;
    settings_copy(&cfg_snapshot);
    const app_settings_t *cfg = &cfg_snapshot;
    cJSON_AddStringToObject(root, "timezone", cfg->timezone);
    cJSON_AddNumberToObject(root, "max_schedules", APP_MAX_SCHEDULES);
    cJSON_AddNumberToObject(root, "max_schedules_per_target", APP_MAX_SCHEDULES_PER_TARGET);
    cJSON *schedules = cJSON_AddArrayToObject(root, "schedules");
    for (size_t i = 0; i < cfg->schedule_count && i < APP_MAX_SCHEDULES; ++i) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddBoolToObject(entry, "enabled", cfg->schedules[i].enabled != 0);
        cJSON_AddNumberToObject(entry, "target", cfg->schedules[i].target);
        cJSON_AddNumberToObject(entry, "hour", cfg->schedules[i].hour);
        cJSON_AddNumberToObject(entry, "minute", cfg->schedules[i].minute);
        cJSON_AddNumberToObject(entry, "days", cfg->schedules[i].days);
        cJSON_AddNumberToObject(entry, "retry_count", cfg->schedules[i].retry_count);
        cJSON *offsets = cJSON_AddArrayToObject(entry, "retry_offsets");
        for (size_t retry = 0; retry < cfg->schedules[i].retry_count; ++retry) {
            cJSON_AddItemToArray(offsets, cJSON_CreateNumber(cfg->schedules[i].retry_offsets_minutes[retry]));
        }
        cJSON_AddNumberToObject(entry, "catchup_window", cfg->schedules[i].catchup_window_minutes);
        cJSON_AddItemToArray(schedules, entry);
    }
#if LIGHTGW_HAS_SCHEDULER
    scheduler_status_t scheduler;
    scheduler_get_status(&scheduler);
    cJSON *scheduler_json = cJSON_AddObjectToObject(root, "scheduler");
    cJSON_AddNumberToObject(scheduler_json, "next_close", scheduler.next_schedule_epoch);
    cJSON_AddNumberToObject(scheduler_json, "last_action", scheduler.last_action_epoch);
    cJSON_AddStringToObject(scheduler_json, "last_result", scheduler.last_result);
    cJSON *logs = cJSON_AddArrayToObject(scheduler_json, "logs");
    for (size_t i = 0; i < scheduler.log_count; ++i) {
        cJSON *log = cJSON_CreateObject();
        cJSON_AddNumberToObject(log, "epoch", scheduler.logs[i].epoch);
        cJSON_AddNumberToObject(log, "schedule_index", scheduler.logs[i].schedule_index);
        cJSON_AddStringToObject(log, "message", scheduler.logs[i].message);
        cJSON_AddItemToArray(logs, log);
    }
#endif

    cJSON_AddArrayToObject(root, "devices");
#if LIGHTGW_HAS_ZIGBEE
    cJSON *array = cJSON_GetObjectItem(root, "devices");
    zb_device_snapshot_t devices[8];
    size_t count = zigbee_manager_snapshot(devices, 8);
    for (size_t i = 0; i < count; ++i) {
        cJSON *d = cJSON_CreateObject();
        char ieee[24];
        snprintf(ieee, sizeof(ieee), "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                 devices[i].ieee_addr[7], devices[i].ieee_addr[6], devices[i].ieee_addr[5], devices[i].ieee_addr[4],
                 devices[i].ieee_addr[3], devices[i].ieee_addr[2], devices[i].ieee_addr[1], devices[i].ieee_addr[0]);
        cJSON_AddNumberToObject(d, "short", devices[i].short_addr);
        cJSON_AddStringToObject(d, "ieee", ieee);
        cJSON_AddStringToObject(d, "model", devices[i].model);
        cJSON_AddBoolToObject(d, "supported", devices[i].supported);
        cJSON_AddBoolToObject(d, "mapping_confirmed", devices[i].mapping_confirmed);
        cJSON_AddBoolToObject(d, "online", devices[i].online);
        cJSON_AddNumberToObject(d, "rssi", devices[i].last_rssi);
        cJSON *states = cJSON_AddArrayToObject(d, "states");
        cJSON *state_details = cJSON_AddArrayToObject(d, "state_details");
        for (int ch = 0; ch < 3; ++ch) {
            cJSON_AddItemToArray(states, cJSON_CreateNumber(devices[i].state[ch]));
            zb_channel_status_t channel = {0};
            zigbee_manager_get_channel_status(devices[i].short_addr, ch, &channel);
            cJSON *detail = cJSON_CreateObject();
            cJSON_AddNumberToObject(detail, "endpoint", channel.endpoint);
            cJSON_AddBoolToObject(detail, "valid", channel.valid);
            cJSON_AddBoolToObject(detail, "read_pending", channel.read_pending);
            cJSON_AddBoolToObject(detail, "read_timed_out", channel.read_timed_out);
            cJSON_AddNumberToObject(detail, "updated_uptime_ms", channel.updated_us / 1000);
            cJSON_AddItemToArray(state_details, detail);
        }
        cJSON *discovered = cJSON_AddArrayToObject(d, "discovered_endpoints");
        for (int ep = 0; ep < devices[i].discovered_endpoint_count; ++ep) {
            cJSON_AddItemToArray(discovered, cJSON_CreateNumber(devices[i].discovered_endpoints[ep]));
        }
        cJSON *onoff = cJSON_AddArrayToObject(d, "onoff_server_endpoints");
        cJSON *onoff_states = cJSON_AddArrayToObject(d, "onoff_endpoint_states");
        for (int ep = 0; ep < devices[i].onoff_endpoint_count; ++ep) {
            uint8_t endpoint = devices[i].onoff_endpoints[ep];
            cJSON_AddItemToArray(onoff, cJSON_CreateNumber(endpoint));
            zb_channel_status_t endpoint_status = {0};
            zigbee_manager_get_endpoint_status(devices[i].short_addr, endpoint, &endpoint_status);
            cJSON *endpoint_json = cJSON_CreateObject();
            cJSON_AddNumberToObject(endpoint_json, "endpoint", endpoint);
            cJSON_AddNumberToObject(endpoint_json, "state", endpoint_status.state);
            cJSON_AddBoolToObject(endpoint_json, "valid", endpoint_status.valid);
            cJSON_AddBoolToObject(endpoint_json, "read_pending", endpoint_status.read_pending);
            cJSON_AddBoolToObject(endpoint_json, "read_timed_out", endpoint_status.read_timed_out);
            cJSON_AddNumberToObject(endpoint_json, "updated_uptime_ms", endpoint_status.updated_us / 1000);
            cJSON_AddItemToArray(onoff_states, endpoint_json);
        }
        cJSON_AddItemToArray(array, d);
    }
#endif
#if LIGHTGW_HAS_IR
    ir_status_t ir;
    ir_manager_get_status(&ir);
    cJSON *ir_json = cJSON_AddObjectToObject(root, "ir");
    cJSON_AddBoolToObject(ir_json, "initialized", ir.initialized);
    cJSON_AddNumberToObject(ir_json, "tx_gpio", APP_IR_TX_GPIO);
    cJSON_AddNumberToObject(ir_json, "rx_gpio", APP_IR_RX_GPIO);
    cJSON_AddBoolToObject(ir_json, "learning", ir.learning);
    cJSON_AddBoolToObject(ir_json, "transmitting", ir.transmitting);
    cJSON_AddNumberToObject(ir_json, "learning_slot", ir.learning_slot);
    cJSON_AddNumberToObject(ir_json, "stable_captures", ir.stable_captures);
    cJSON_AddNumberToObject(ir_json, "required_captures", IR_CAPTURE_REQUIRED);
    cJSON_AddNumberToObject(ir_json, "attempts", ir.attempts);
    cJSON_AddBoolToObject(ir_json, "candidate_ready", ir.candidate_ready);
    cJSON_AddNumberToObject(ir_json, "candidate_duration_count", ir.candidate_duration_count);
    cJSON_AddNumberToObject(ir_json, "candidate_total_us", ir.candidate_total_us);
    cJSON_AddStringToObject(ir_json, "candidate_protocol", ir_protocol_name(ir.candidate_protocol));
    cJSON_AddStringToObject(ir_json, "carrier_note", "configured/estimated; not measured by the 38kHz demodulating receiver");
    cJSON *slots = cJSON_AddArrayToObject(ir_json, "slots");
    for (int i = 0; i < IR_CODE_SLOT_COUNT; ++i) {
        cJSON *slot = cJSON_CreateObject();
        cJSON_AddBoolToObject(slot, "saved", ir.code_saved[i]);
        cJSON_AddNumberToObject(slot, "duration_count", ir.duration_count[i]);
        cJSON_AddNumberToObject(slot, "carrier_hz", ir.carrier_hz[i]);
        cJSON_AddNumberToObject(slot, "duty_percent", ir.duty_percent[i]);
        cJSON_AddStringToObject(slot, "protocol", ir_protocol_name(ir.protocol[i]));
        cJSON_AddNumberToObject(slot, "crc32", ir.code_crc[i]);
        cJSON_AddItemToArray(slots, slot);
    }
    cJSON_AddNumberToObject(ir_json, "last_send_epoch", ir.last_send_epoch);
    cJSON_AddNumberToObject(ir_json, "last_send_uptime_seconds", ir.last_send_uptime_seconds);
    cJSON_AddNumberToObject(ir_json, "last_send_slot_mask", ir.last_send_slot_mask);
    cJSON_AddStringToObject(ir_json, "last_send_result", ir.last_send_result);
    cJSON_AddStringToObject(ir_json, "schedule_status", ir.schedule_skip_reason);
    cJSON *ir_schedule = cJSON_AddObjectToObject(ir_json, "schedule");
    cJSON_AddBoolToObject(ir_schedule, "enabled", ir.schedule.enabled != 0);
    cJSON_AddBoolToObject(ir_schedule, "use_light_schedules", ir.schedule.use_light_schedules != 0);
    cJSON_AddNumberToObject(ir_schedule, "hour", ir.schedule.hour);
    cJSON_AddNumberToObject(ir_schedule, "minute", ir.schedule.minute);
    cJSON_AddNumberToObject(ir_schedule, "days", ir.schedule.days);
    cJSON_AddNumberToObject(ir_schedule, "slot_mask", ir.schedule.slot_mask);
#endif
    char *text = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    cJSON_Delete(root);
    return err;
}

static esp_err_t resources_get(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    diagnostics_refresh_task_stats();
    diagnostics_heap_t heap;
    diagnostics_get_heap(&heap);
    storage_manager_refresh_stats();
    storage_status_t storage;
    storage_manager_get_status(&storage);
    bool truncated = false, enabled = false;
    size_t task_count = diagnostics_task_count(&truncated, &enabled);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddNumberToObject(root, "free_heap", heap.free_heap);
    cJSON_AddNumberToObject(root, "minimum_free_heap", heap.minimum_free_heap);
    cJSON_AddNumberToObject(root, "largest_free_block", heap.largest_free_block);
    cJSON_AddBoolToObject(root, "task_monitor_enabled", enabled);
    cJSON_AddBoolToObject(root, "tasks_truncated", truncated);
    cJSON *storage_json = cJSON_AddObjectToObject(root, "storage");
    cJSON_AddBoolToObject(storage_json, "degraded", storage.degraded);
    cJSON_AddNumberToObject(storage_json, "write_attempts", storage.write_attempts);
    cJSON_AddNumberToObject(storage_json, "writes_committed", storage.writes_committed);
    cJSON_AddNumberToObject(storage_json, "writes_skipped", storage.writes_skipped);
    cJSON_AddNumberToObject(storage_json, "write_failures", storage.write_failures);
    cJSON_AddNumberToObject(storage_json, "corruption_events", storage.corruption_events);
    cJSON_AddStringToObject(storage_json, "last_error", storage.last_error);
    cJSON_AddStringToObject(storage_json, "last_recovery", storage.last_recovery);
    cJSON *partitions = cJSON_AddArrayToObject(storage_json, "partitions");
    const storage_partition_status_t *partition_status[] = {
        &storage.primary, &storage.runtime, &storage.ir, &storage.sensor, &storage.cloud,
    };
    for (size_t i = 0; i < sizeof(partition_status) / sizeof(partition_status[0]); ++i) {
        const storage_partition_status_t *p = partition_status[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", p->name);
        cJSON_AddBoolToObject(item, "initialized", p->initialized);
        cJSON_AddBoolToObject(item, "stats_valid", p->stats_valid);
        cJSON_AddNumberToObject(item, "used_entries", p->used_entries);
        cJSON_AddNumberToObject(item, "free_entries", p->free_entries);
        cJSON_AddNumberToObject(item, "total_entries", p->total_entries);
        cJSON_AddNumberToObject(item, "namespace_count", p->namespace_count);
        cJSON_AddNumberToObject(item, "last_error", p->last_error);
        cJSON_AddItemToArray(partitions, item);
    }
    cJSON *tasks = cJSON_AddArrayToObject(root, "tasks");
    for (size_t i = 0; i < task_count; ++i) {
        diagnostics_task_stat_t stat;
        if (!diagnostics_get_task_stat(i, &stat)) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", stat.name);
        cJSON_AddStringToObject(item, "state", stat.state);
        cJSON_AddNumberToObject(item, "stack_high_water_words", stat.stack_high_water_words);
        cJSON_AddNumberToObject(item, "stack_high_water_bytes", stat.stack_high_water_bytes);
        cJSON_AddNumberToObject(item, "priority", stat.priority);
        cJSON_AddNumberToObject(item, "core_id", stat.core_id);
        cJSON_AddItemToArray(tasks, item);
    }
    cJSON *events = cJSON_AddArrayToObject(root, "memory_events");
    size_t event_count = diagnostics_memory_event_count();
    for (size_t i = 0; i < event_count; ++i) {
        diagnostics_memory_event_t event;
        if (!diagnostics_get_memory_event(i, &event)) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", event.name);
        cJSON_AddNumberToObject(item, "uptime_ms", event.uptime_ms);
        cJSON_AddNumberToObject(item, "free_heap", event.heap.free_heap);
        cJSON_AddNumberToObject(item, "minimum_free_heap", event.heap.minimum_free_heap);
        cJSON_AddNumberToObject(item, "largest_free_block", event.heap.largest_free_block);
        cJSON_AddItemToArray(events, item);
    }
    char *text = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = text ? httpd_resp_sendstr(req, text) : ESP_ERR_NO_MEM;
    free(text);
    cJSON_Delete(root);
    return err;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t factory_reset_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *j = receive_json(req);
    cJSON *confirm = j ? cJSON_GetObjectItem(j, "confirm") : NULL;
    bool confirmed = cJSON_IsString(confirm) &&
                     strcmp(confirm->valuestring, "ERASE_ALL_USER_DATA") == 0;
    cJSON_Delete(j);
    if (!confirmed) {
        return send_json_error(req, "400 Bad Request",
                               "Type ERASE_ALL_USER_DATA to explicitly confirm factory reset");
    }
    esp_err_t err = storage_manager_factory_reset("ERASE_ALL_USER_DATA");
    if (err != ESP_OK) return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restarting\":true,\"destructive\":true}");
    xTaskCreate(restart_task, "factory_restart", 2048, NULL, 3, NULL);
    return ESP_OK;
}

static esp_err_t provision_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    if (!wifi_manager_is_provisioning()) return send_json_error(req, "403 Forbidden", "Provisioning is closed");
    cJSON *j = receive_json(req);
    cJSON *ssid = j ? cJSON_GetObjectItem(j, "ssid") : NULL;
    cJSON *pass = j ? cJSON_GetObjectItem(j, "password") : NULL;
    cJSON *user = j ? cJSON_GetObjectItem(j, "admin_user") : NULL;
    cJSON *admin = j ? cJSON_GetObjectItem(j, "admin_password") : NULL;
    cJSON *device_name = j ? cJSON_GetObjectItem(j, "device_name") : NULL;
    if (!cJSON_IsString(ssid) || !cJSON_IsString(pass) || !cJSON_IsString(user) ||
        !cJSON_IsString(admin) || !cJSON_IsString(device_name)) {
        cJSON_Delete(j);
        return send_json_error(req, "400 Bad Request", "Missing fields");
    }
    esp_err_t err = settings_save_device_name(device_name->valuestring);
    if (err == ESP_OK) err = settings_save_admin(user->valuestring, admin->valuestring);
    if (err == ESP_OK) err = settings_save_wifi(ssid->valuestring, pass->valuestring);
    cJSON_Delete(j);
    if (err != ESP_OK) return send_json_error(req, "400 Bad Request", "Invalid settings (device name: lowercase letters/digits/hyphens; admin password: at least 12 characters)");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restarting\":true}");
    xTaskCreate(restart_task, "restart", 2048, NULL, 3, NULL);
    return ESP_OK;
}

static esp_err_t wifi_reconfigure_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *j = receive_json(req);
    cJSON *confirm = j ? cJSON_GetObjectItem(j, "confirm") : NULL;
    bool confirmed = cJSON_IsString(confirm) && strcmp(confirm->valuestring, "RECONFIGURE") == 0;
    cJSON_Delete(j);
    if (!confirmed) return send_json_error(req, "400 Bad Request", "Explicit confirmation is required");
    esp_err_t err = settings_clear_wifi();
    if (err != ESP_OK) return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    ESP_LOGW(TAG, "Authenticated Wi-Fi reconfiguration requested; restarting into temporary provisioning AP");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restarting\":true}");
    xTaskCreate(restart_task, "restart", 2048, NULL, 3, NULL);
    return ESP_OK;
}

static esp_err_t time_ntp_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    esp_err_t err = time_manager_request_ntp();
    return err == ESP_OK ? httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"NTP synchronization queued\"}") :
                           send_json_error(req, "409 Conflict", esp_err_to_name(err));
}

static esp_err_t time_manual_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *j = receive_json(req);
    const char *names[] = {"year", "month", "day", "hour", "minute", "second"};
    int values[6];
    bool valid = j != NULL;
    for (size_t i = 0; i < 6 && valid; ++i) {
        cJSON *item = cJSON_GetObjectItem(j, names[i]);
        valid = cJSON_IsNumber(item) && item->valuedouble == item->valueint;
        if (valid) values[i] = item->valueint;
    }
    if (!valid) {
        cJSON_Delete(j);
        return send_json_error(req, "400 Bad Request", "year/month/day/hour/minute/second integers required");
    }
    esp_err_t err = time_manager_set_local(values[0], values[1], values[2], values[3], values[4], values[5]);
    cJSON_Delete(j);
    return err == ESP_OK ? httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"ESP32 time set; inspect RTC status for DS3231 write result\"}") :
                           send_json_error(req, "400 Bad Request", esp_err_to_name(err));
}

static void add_extension_module_json(cJSON *parent, const char *name,
                                      const extension_module_status_t *status)
{
    cJSON *item = cJSON_AddObjectToObject(parent, name);
    cJSON_AddBoolToObject(item, "included", status->included);
    cJSON_AddBoolToObject(item, "enabled", status->enabled);
    cJSON_AddBoolToObject(item, "running", status->running);
    cJSON_AddBoolToObject(item, "online", status->online);
    cJSON_AddBoolToObject(item, "data_valid", status->data_valid);
    cJSON_AddBoolToObject(item, "restart_required", status->restart_required);
    cJSON_AddNumberToObject(item, "last_success_uptime_ms", status->last_success_uptime_ms);
    cJSON_AddStringToObject(item, "last_error", status->last_error);
}

static esp_err_t extensions_get(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    extension_manager_snapshot_t snapshot;
    extension_manager_get_snapshot(&snapshot);
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddTrueToObject(root, "ok");
    cJSON *modules = cJSON_AddObjectToObject(root, "modules");
    add_extension_module_json(modules, "bh1750", &snapshot.modules[EXTENSION_MODULE_BH1750]);
    add_extension_module_json(modules, "sht31", &snapshot.modules[EXTENSION_MODULE_SHT31]);
    add_extension_module_json(modules, "rainmaker", &snapshot.modules[EXTENSION_MODULE_RAINMAKER]);
    cJSON *rainmaker = cJSON_AddObjectToObject(root, "rainmaker");
    cJSON_AddBoolToObject(rainmaker, "claimed", snapshot.rainmaker.claimed);
    cJSON_AddBoolToObject(rainmaker, "mapping_in_progress", snapshot.rainmaker.mapping_in_progress);
    cJSON_AddStringToObject(rainmaker, "node_id", snapshot.rainmaker.node_id);

    cJSON *bh = cJSON_AddObjectToObject(root, "bh1750");
    cJSON_AddNumberToObject(bh, "lux", snapshot.bh1750.lux);
    cJSON_AddNumberToObject(bh, "filtered_lux", snapshot.bh1750.filtered_lux);
    cJSON_AddBoolToObject(bh, "night_anomaly", snapshot.bh1750.night_anomaly);
    cJSON_AddNumberToObject(bh, "sample_period_seconds", snapshot.bh1750.sample_period_seconds);
    cJSON_AddNumberToObject(bh, "night_start_minute", snapshot.bh1750.night_start_minute);
    cJSON_AddNumberToObject(bh, "night_end_minute", snapshot.bh1750.night_end_minute);
    cJSON_AddNumberToObject(bh, "night_lux_threshold", snapshot.bh1750.night_lux_threshold);
    cJSON_AddNumberToObject(bh, "consecutive_anomaly_limit", snapshot.bh1750.consecutive_anomaly_limit);
    cJSON_AddNumberToObject(bh, "calibration_valid_mask", snapshot.bh1750.calibration_valid_mask);
    cJSON *calibration = cJSON_AddArrayToObject(bh, "calibration_lux");
    for (size_t i = 0; i < 5; ++i) {
        cJSON_AddItemToArray(calibration, cJSON_CreateNumber(snapshot.bh1750.calibration_lux[i]));
    }
    cJSON *bh_history = cJSON_AddArrayToObject(bh, "history");
    for (size_t i = 0; i < snapshot.bh1750.history_count; ++i) {
        cJSON *sample = cJSON_CreateObject();
        cJSON_AddNumberToObject(sample, "uptime_ms", snapshot.bh1750.history[i].uptime_ms);
        cJSON_AddNumberToObject(sample, "lux", snapshot.bh1750.history[i].value_1);
        cJSON_AddNumberToObject(sample, "filtered_lux", snapshot.bh1750.history[i].value_2);
        cJSON_AddItemToArray(bh_history, sample);
    }

    cJSON *sht = cJSON_AddObjectToObject(root, "sht31");
    cJSON_AddNumberToObject(sht, "temperature_c", snapshot.sht31.temperature_c);
    cJSON_AddNumberToObject(sht, "humidity_percent", snapshot.sht31.humidity_percent);
    cJSON_AddStringToObject(sht, "temperature_trend", snapshot.sht31.temperature_trend ?: "UNKNOWN");
    cJSON_AddNumberToObject(sht, "sample_period_seconds", snapshot.sht31.sample_period_seconds);
    cJSON *sht_history = cJSON_AddArrayToObject(sht, "history");
    for (size_t i = 0; i < snapshot.sht31.history_count; ++i) {
        cJSON *sample = cJSON_CreateObject();
        cJSON_AddNumberToObject(sample, "uptime_ms", snapshot.sht31.history[i].uptime_ms);
        cJSON_AddNumberToObject(sample, "temperature_c", snapshot.sht31.history[i].value_1);
        cJSON_AddNumberToObject(sample, "humidity_percent", snapshot.sht31.history[i].value_2);
        cJSON_AddItemToArray(sht_history, sample);
    }
    char *text = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = text ? httpd_resp_sendstr(req, text) : ESP_ERR_NO_MEM;
    free(text);
    cJSON_Delete(root);
    return err;
}

static bool parse_extension_module(const char *name, extension_module_id_t *module)
{
    if (!name || !module) return false;
    if (strcmp(name, "bh1750") == 0) *module = EXTENSION_MODULE_BH1750;
    else if (strcmp(name, "sht31") == 0) *module = EXTENSION_MODULE_SHT31;
    else if (strcmp(name, "rainmaker") == 0) *module = EXTENSION_MODULE_RAINMAKER;
    else return false;
    return true;
}

static esp_err_t extension_toggle_post(httpd_req_t *req)
{
    if (!backup_authorized(req)) return ESP_OK;
    cJSON *json = receive_json(req);
    cJSON *name = json ? cJSON_GetObjectItem(json, "module") : NULL;
    cJSON *enabled = json ? cJSON_GetObjectItem(json, "enabled") : NULL;
    extension_module_id_t module;
    if (!cJSON_IsString(name) || !cJSON_IsBool(enabled) ||
        !parse_extension_module(name->valuestring, &module)) {
        cJSON_Delete(json);
        return send_json_error(req, "400 Bad Request", "module and boolean enabled are required");
    }
    bool restart_required = false;
    esp_err_t err = extension_manager_set_enabled(module, cJSON_IsTrue(enabled), &restart_required);
    cJSON_Delete(json);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return send_json_error(req, "409 Conflict", "Module is not included in this firmware");
    }
    if (err != ESP_OK) return send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddTrueToObject(reply, "ok");
    cJSON_AddBoolToObject(reply, "restart_required", restart_required);
    cJSON_AddStringToObject(reply, "message", restart_required ?
                            "Setting saved; a controlled restart is required" :
                            "Module state changed without restarting Core");
    char *text = cJSON_PrintUnformatted(reply);
    httpd_resp_set_type(req, "application/json");
    err = text ? httpd_resp_sendstr(req, text) : ESP_ERR_NO_MEM;
    free(text);
    cJSON_Delete(reply);
    return err;
}

static bool json_integer_in_range(cJSON *item, int minimum, int maximum)
{
    return cJSON_IsNumber(item) && item->valuedouble == item->valueint &&
           item->valueint >= minimum && item->valueint <= maximum;
}

static esp_err_t bh1750_config_post(httpd_req_t *req)
{
    if (!backup_authorized(req)) return ESP_OK;
    cJSON *json = receive_json(req);
    cJSON *period = json ? cJSON_GetObjectItem(json, "sample_period_seconds") : NULL;
    cJSON *start = json ? cJSON_GetObjectItem(json, "night_start_minute") : NULL;
    cJSON *end = json ? cJSON_GetObjectItem(json, "night_end_minute") : NULL;
    cJSON *threshold = json ? cJSON_GetObjectItem(json, "night_lux_threshold") : NULL;
    cJSON *limit = json ? cJSON_GetObjectItem(json, "consecutive_anomaly_limit") : NULL;
    if (!json_integer_in_range(period, 10, 300) || !json_integer_in_range(start, 0, 1439) ||
        !json_integer_in_range(end, 0, 1439) || !cJSON_IsNumber(threshold) ||
        threshold->valuedouble < 0.0 || threshold->valuedouble > 10000.0 ||
        !json_integer_in_range(limit, 1, 20)) {
        cJSON_Delete(json);
        return send_json_error(req, "400 Bad Request", "Invalid BH1750 configuration");
    }
    extension_bh1750_view_t config = {
        .sample_period_seconds = period->valueint,
        .night_start_minute = start->valueint,
        .night_end_minute = end->valueint,
        .night_lux_threshold = (float)threshold->valuedouble,
        .consecutive_anomaly_limit = limit->valueint,
    };
    esp_err_t err = extension_manager_set_bh1750_config(&config);
    cJSON_Delete(json);
    if (err == ESP_ERR_NOT_SUPPORTED) return send_json_error(req, "409 Conflict", "BH1750 is not included");
    return err == ESP_OK ? httpd_resp_sendstr(req, "{\"ok\":true}") :
                           send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
}

static esp_err_t bh1750_calibrate_post(httpd_req_t *req)
{
    if (!backup_authorized(req)) return ESP_OK;
    cJSON *json = receive_json(req);
    cJSON *scene = json ? cJSON_GetObjectItem(json, "scene") : NULL;
    if (!json_integer_in_range(scene, 0, 4)) {
        cJSON_Delete(json);
        return send_json_error(req, "400 Bad Request", "scene must be 0..4");
    }
    esp_err_t err = extension_manager_record_bh1750_calibration(scene->valueint);
    cJSON_Delete(json);
    if (err == ESP_ERR_NOT_SUPPORTED) return send_json_error(req, "409 Conflict", "BH1750 is not included");
    if (err == ESP_ERR_INVALID_STATE) return send_json_error(req, "409 Conflict", "No valid current Lux sample");
    return err == ESP_OK ? httpd_resp_sendstr(req, "{\"ok\":true}") :
                           send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
}

static esp_err_t sht31_config_post(httpd_req_t *req)
{
    if (!backup_authorized(req)) return ESP_OK;
    cJSON *json = receive_json(req);
    cJSON *period = json ? cJSON_GetObjectItem(json, "sample_period_seconds") : NULL;
    if (!json_integer_in_range(period, 30, 60)) {
        cJSON_Delete(json);
        return send_json_error(req, "400 Bad Request", "SHT31 period must be 30..60 seconds");
    }
    esp_err_t err = extension_manager_set_sht31_config(period->valueint);
    cJSON_Delete(json);
    if (err == ESP_ERR_NOT_SUPPORTED) return send_json_error(req, "409 Conflict", "SHT31 is not included");
    return err == ESP_OK ? httpd_resp_sendstr(req, "{\"ok\":true}") :
                           send_json_error(req, "500 Internal Server Error", esp_err_to_name(err));
}

static esp_err_t rainmaker_mapping_post(httpd_req_t *req)
{
    if (!backup_authorized(req)) return ESP_OK;
    cJSON *json = receive_json(req);
    cJSON *user_id = json ? cJSON_GetObjectItem(json, "user_id") : NULL;
    cJSON *secret = json ? cJSON_GetObjectItem(json, "secret_key") : NULL;
    if (!cJSON_IsString(user_id) || !cJSON_IsString(secret) ||
        strlen(user_id->valuestring) < 3 || strlen(user_id->valuestring) > 64 ||
        strlen(secret->valuestring) < 8 || strlen(secret->valuestring) > 64) {
        cJSON_Delete(json);
        return send_json_error(req, "400 Bad Request", "user_id/secret_key length is invalid");
    }
    char user_copy[65], secret_copy[65];
    strlcpy(user_copy, user_id->valuestring, sizeof(user_copy));
    strlcpy(secret_copy, secret->valuestring, sizeof(secret_copy));
    memset(user_id->valuestring, 0, strlen(user_id->valuestring));
    memset(secret->valuestring, 0, strlen(secret->valuestring));
    cJSON_Delete(json);
    esp_err_t err = extension_manager_start_rainmaker_mapping(user_copy, secret_copy);
    memset(user_copy, 0, sizeof(user_copy));
    memset(secret_copy, 0, sizeof(secret_copy));
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return send_json_error(req, "409 Conflict", "RainMaker is not included in this firmware");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_error(req, "409 Conflict", "RainMaker must be enabled and MQTT online");
    }
    return err == ESP_OK ? httpd_resp_sendstr(req,
        "{\"ok\":true,\"message\":\"Mapping request submitted; verify completion in status\"}") :
        send_json_error(req, "502 Bad Gateway", esp_err_to_name(err));
}

#if LIGHTGW_HAS_ZIGBEE
static esp_err_t pair_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *j = receive_json(req);
    cJSON *seconds = j ? cJSON_GetObjectItem(j, "seconds") : NULL;
    int value = cJSON_IsNumber(seconds) ? seconds->valueint : 180;
    cJSON_Delete(j);
    if (value < 1 || value > 180) return send_json_error(req, "400 Bad Request", "Pairing window must be 1..180 seconds");
    esp_err_t err = zigbee_manager_permit_join(value);
    return err == ESP_OK ? httpd_resp_sendstr(req, "{\"ok\":true}") : send_json_error(req, "503 Service Unavailable", esp_err_to_name(err));
}

static esp_err_t zigbee_diagnostic_read_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *j = receive_json(req);
    cJSON *addr = j ? cJSON_GetObjectItem(j, "short") : NULL;
    if (!cJSON_IsNumber(addr) || addr->valuedouble != addr->valueint ||
        addr->valueint < 0 || addr->valueint > UINT16_MAX) {
        cJSON_Delete(j);
        return send_json_error(req, "400 Bad Request", "valid short address is required");
    }
    uint16_t short_addr = (uint16_t)addr->valueint;
    cJSON_Delete(j);
    esp_err_t err = zigbee_manager_diagnostic_read(short_addr);
    return err == ESP_OK ? httpd_resp_sendstr(req,
        "{\"ok\":true,\"message\":\"Read-only endpoint refresh submitted; no ON/OFF command was sent\"}") :
        send_json_error(req, "409 Conflict", esp_err_to_name(err));
}
#endif

#if LIGHTGW_HAS_IR
static esp_err_t ir_learn_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *j = receive_json(req);
    cJSON *slot = j ? cJSON_GetObjectItem(j, "slot") : NULL;
    if (!cJSON_IsNumber(slot) || slot->valuedouble != slot->valueint || slot->valueint < 0 || slot->valueint >= IR_CODE_SLOT_COUNT) {
        cJSON_Delete(j);
        return send_json_error(req, "400 Bad Request", "slot must be 0 or 1");
    }
    esp_err_t err = ir_manager_start_learning(slot->valueint);
    cJSON_Delete(j);
    return err == ESP_OK ? httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Press the explicit OFF key three times\"}") :
                           send_json_error(req, "409 Conflict", esp_err_to_name(err));
}

static esp_err_t ir_save_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *j = receive_json(req);
    cJSON *slot = j ? cJSON_GetObjectItem(j, "slot") : NULL;
    cJSON *carrier = j ? cJSON_GetObjectItem(j, "carrier_hz") : NULL;
    cJSON *duty = j ? cJSON_GetObjectItem(j, "duty_percent") : NULL;
    if (!cJSON_IsNumber(slot) || !cJSON_IsNumber(carrier) || !cJSON_IsNumber(duty) ||
        slot->valuedouble != slot->valueint || carrier->valuedouble != carrier->valueint ||
        duty->valuedouble != duty->valueint ||
        slot->valueint < 0 || slot->valueint >= IR_CODE_SLOT_COUNT || carrier->valueint < 30000 ||
        carrier->valueint > 60000 || duty->valueint < 10 || duty->valueint > 50) {
        cJSON_Delete(j);
        return send_json_error(req, "400 Bad Request", "invalid slot/carrier/duty");
    }
    esp_err_t err = ir_manager_save_candidate(slot->valueint, carrier->valueint, duty->valueint);
    cJSON_Delete(j);
    return err == ESP_OK ? httpd_resp_sendstr(req, "{\"ok\":true}") :
                           send_json_error(req, "409 Conflict", esp_err_to_name(err));
}

static esp_err_t ir_send_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *j = receive_json(req);
    cJSON *mask = j ? cJSON_GetObjectItem(j, "slot_mask") : NULL;
    cJSON *repeats = j ? cJSON_GetObjectItem(j, "repeats") : NULL;
    if (!cJSON_IsNumber(mask) || !cJSON_IsNumber(repeats) || mask->valuedouble != mask->valueint ||
        repeats->valuedouble != repeats->valueint || mask->valueint < 1 || mask->valueint > 3 ||
        repeats->valueint < 1 || repeats->valueint > 2) {
        cJSON_Delete(j);
        return send_json_error(req, "400 Bad Request", "slot_mask must be 1..3 and repeats 1..2");
    }
    schedule_target_t target = mask->valueint == 1 ? SCHEDULE_TARGET_AC_1 :
                               mask->valueint == 2 ? SCHEDULE_TARGET_AC_2 : SCHEDULE_TARGET_AC_GROUP;
    uint32_t command_id = 0;
    esp_err_t err = device_control_submit_target_repeated(target, repeats->valueint, "web_ir", &command_id);
    cJSON_Delete(j);
    return err == ESP_OK ? send_command_accepted(req, command_id) :
                           send_json_error(req, "409 Conflict", esp_err_to_name(err));
}

#endif

#if LIGHTGW_HAS_CONTROL
static esp_err_t mapping_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *j = receive_json(req);
    cJSON *addr = j ? cJSON_GetObjectItem(j, "short") : NULL;
    cJSON *left = j ? cJSON_GetObjectItem(j, "left") : NULL;
    cJSON *center = j ? cJSON_GetObjectItem(j, "center") : NULL;
    cJSON *right = j ? cJSON_GetObjectItem(j, "right") : NULL;
    if (!cJSON_IsNumber(addr) || !cJSON_IsNumber(left) || !cJSON_IsNumber(center) || !cJSON_IsNumber(right)) {
        cJSON_Delete(j);
        return send_json_error(req, "400 Bad Request", "short/left/center/right are required");
    }
    uint8_t endpoints[3] = {left->valueint, center->valueint, right->valueint};
    esp_err_t err = zigbee_manager_confirm_mapping(addr->valueint, endpoints);
    cJSON_Delete(j);
    return err == ESP_OK ? httpd_resp_sendstr(req, "{\"ok\":true}") :
                           send_json_error(req, "409 Conflict", esp_err_to_name(err));
}

static esp_err_t switch_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *j = receive_json(req);
    cJSON *addr = j ? cJSON_GetObjectItem(j, "short") : NULL;
    cJSON *channel = j ? cJSON_GetObjectItem(j, "channel") : NULL;
    cJSON *on = j ? cJSON_GetObjectItem(j, "on") : NULL;
    if (!cJSON_IsNumber(addr) || !cJSON_IsNumber(channel) || !cJSON_IsBool(on)) {
        cJSON_Delete(j);
        return send_json_error(req, "400 Bad Request", "Invalid switch request");
    }
    uint32_t command_id = 0;
    esp_err_t err = device_control_submit_light(addr->valueint, channel->valueint, cJSON_IsTrue(on),
                                                "web", &command_id);
    cJSON_Delete(j);
    return err == ESP_OK ? send_command_accepted(req, command_id) :
                           send_json_error(req, "409 Conflict", esp_err_to_name(err));
}

static esp_err_t all_off_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    uint32_t command_id = 0;
    esp_err_t err = device_control_submit_target(SCHEDULE_TARGET_LIGHT_ALL, "web_all_off", &command_id);
    return err == ESP_OK ? send_command_accepted(req, command_id) :
                           send_json_error(req, "503 Service Unavailable", esp_err_to_name(err));
}

static esp_err_t control_result_get(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    char query[48], value[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", value, sizeof(value)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "id query parameter required");
    }
    char *end = NULL;
    unsigned long id = strtoul(value, &end, 10);
    if (!id || !end || *end) return send_json_error(req, "400 Bad Request", "invalid command id");
    device_command_result_t result;
    esp_err_t err = device_control_get_result((uint32_t)id, &result);
    if (err != ESP_OK) return send_json_error(req, "404 Not Found", "command result expired or not found");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddNumberToObject(root, "command_id", result.id);
    cJSON_AddStringToObject(root, "status", device_command_status_name(result.status));
    cJSON_AddBoolToObject(root, "terminal", result.terminal);
    cJSON_AddNumberToObject(root, "actual_state", result.actual_state);
    cJSON_AddStringToObject(root, "detail", result.detail);
    char *text = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_sendstr(req, text);
    free(text);
    cJSON_Delete(root);
    return err;
}
#endif

#if LIGHTGW_HAS_SCHEDULER
static esp_err_t schedule_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *j = receive_json(req);
    cJSON *tz = j ? cJSON_GetObjectItem(j, "timezone") : NULL;
    cJSON *items = j ? cJSON_GetObjectItem(j, "schedules") : NULL;
    int count = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : -1;
    if (!cJSON_IsString(tz) || count < 0 || count > APP_MAX_SCHEDULES) {
        cJSON_Delete(j);
        return send_json_error(req, "400 Bad Request", "Invalid schedule");
    }
    app_schedule_entry_t entries[APP_MAX_SCHEDULES] = {0};
    for (int i = 0; i < count; ++i) {
        cJSON *item = cJSON_GetArrayItem(items, i);
        cJSON *enabled = cJSON_GetObjectItem(item, "enabled");
        cJSON *target = cJSON_GetObjectItem(item, "target");
        cJSON *hour = cJSON_GetObjectItem(item, "hour");
        cJSON *minute = cJSON_GetObjectItem(item, "minute");
        cJSON *days = cJSON_GetObjectItem(item, "days");
        cJSON *retry_count = cJSON_GetObjectItem(item, "retry_count");
        cJSON *retry_offsets = cJSON_GetObjectItem(item, "retry_offsets");
        cJSON *catchup = cJSON_GetObjectItem(item, "catchup_window");
        if (!cJSON_IsBool(enabled) || !cJSON_IsNumber(target) || !cJSON_IsNumber(hour) ||
            !cJSON_IsNumber(minute) || !cJSON_IsNumber(days) || !cJSON_IsNumber(retry_count) ||
            !cJSON_IsArray(retry_offsets) || !cJSON_IsNumber(catchup) ||
            target->valuedouble != target->valueint || hour->valuedouble != hour->valueint ||
            minute->valuedouble != minute->valueint || days->valuedouble != days->valueint ||
            retry_count->valuedouble != retry_count->valueint || catchup->valuedouble != catchup->valueint ||
            target->valueint < 0 || target->valueint >= APP_SCHEDULE_TARGET_COUNT ||
#if !LIGHTGW_HAS_IR
            target->valueint >= SCHEDULE_TARGET_AC_1 ||
#endif
            hour->valueint < 0 || hour->valueint > 23 || minute->valueint < 0 || minute->valueint > 59 ||
            days->valueint < 0 || days->valueint > 127 || retry_count->valueint < 0 ||
            retry_count->valueint > APP_MAX_SCHEDULE_RETRIES ||
            cJSON_GetArraySize(retry_offsets) != retry_count->valueint || catchup->valueint < 0 || catchup->valueint > 180) {
            cJSON_Delete(j);
            return send_json_error(req, "400 Bad Request", "Invalid per-device schedule");
        }
        entries[i].enabled = cJSON_IsTrue(enabled);
        entries[i].target = target->valueint;
        entries[i].hour = hour->valueint;
        entries[i].minute = minute->valueint;
        entries[i].days = days->valueint;
        entries[i].retry_count = retry_count->valueint;
        entries[i].catchup_window_minutes = catchup->valueint;
        for (size_t retry = 0; retry < entries[i].retry_count; ++retry) {
            cJSON *offset = cJSON_GetArrayItem(retry_offsets, retry);
            if (!cJSON_IsNumber(offset) || offset->valuedouble != offset->valueint ||
                offset->valueint < 1 || offset->valueint > 180) {
                cJSON_Delete(j);
                return send_json_error(req, "400 Bad Request", "Invalid retry offset");
            }
            entries[i].retry_offsets_minutes[retry] = offset->valueint;
        }
    }
    esp_err_t err = settings_save_schedules(entries, count, tz->valuestring);
    cJSON_Delete(j);
    return err == ESP_OK ? httpd_resp_sendstr(req, "{\"ok\":true}") : send_json_error(req, "400 Bad Request", esp_err_to_name(err));
}
#endif

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 32;
    cfg.stack_size = 8192;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "HTTP server start");
    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/api/auth/info", .method = HTTP_GET, .handler = auth_info_get},
        {.uri = "/api/auth/login", .method = HTTP_POST, .handler = auth_login_post},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/diagnostics/resources", .method = HTTP_GET, .handler = resources_get},
        {.uri = "/api/storage/factory-reset", .method = HTTP_POST, .handler = factory_reset_post},
        {.uri = "/api/backup/export", .method = HTTP_GET, .handler = backup_export_get},
        {.uri = "/api/backup/import/validate", .method = HTTP_POST, .handler = backup_validate_post},
        {.uri = "/api/backup/import/commit", .method = HTTP_POST, .handler = backup_commit_post},
        {.uri = "/api/provision", .method = HTTP_POST, .handler = provision_post},
        {.uri = "/api/wifi/reconfigure", .method = HTTP_POST, .handler = wifi_reconfigure_post},
        {.uri = "/api/time/ntp", .method = HTTP_POST, .handler = time_ntp_post},
        {.uri = "/api/time/manual", .method = HTTP_POST, .handler = time_manual_post},
        {.uri = "/api/extensions", .method = HTTP_GET, .handler = extensions_get},
        {.uri = "/api/extensions/toggle", .method = HTTP_POST, .handler = extension_toggle_post},
        {.uri = "/api/extensions/bh1750/config", .method = HTTP_POST, .handler = bh1750_config_post},
        {.uri = "/api/extensions/bh1750/calibrate", .method = HTTP_POST, .handler = bh1750_calibrate_post},
        {.uri = "/api/extensions/sht31/config", .method = HTTP_POST, .handler = sht31_config_post},
        {.uri = "/api/extensions/rainmaker/map", .method = HTTP_POST, .handler = rainmaker_mapping_post},
#if LIGHTGW_HAS_ZIGBEE
        {.uri = "/api/pair", .method = HTTP_POST, .handler = pair_post},
        {.uri = "/api/zigbee/diagnostic-read", .method = HTTP_POST, .handler = zigbee_diagnostic_read_post},
#endif
#if LIGHTGW_HAS_CONTROL
        {.uri = "/api/mapping", .method = HTTP_POST, .handler = mapping_post},
        {.uri = "/api/switch", .method = HTTP_POST, .handler = switch_post},
        {.uri = "/api/all-off", .method = HTTP_POST, .handler = all_off_post},
        {.uri = "/api/control/result", .method = HTTP_GET, .handler = control_result_get},
#endif
#if LIGHTGW_HAS_SCHEDULER
        {.uri = "/api/schedule", .method = HTTP_POST, .handler = schedule_post},
#endif
#if LIGHTGW_HAS_IR
        {.uri = "/api/ir/learn", .method = HTTP_POST, .handler = ir_learn_post},
        {.uri = "/api/ir/save", .method = HTTP_POST, .handler = ir_save_post},
        {.uri = "/api/ir/send", .method = HTTP_POST, .handler = ir_send_post},
#endif
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &handlers[i]), TAG, "register URI");
    }
    return ESP_OK;
}
