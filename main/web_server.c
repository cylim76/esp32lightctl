#include "web_server.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "diagnostics.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "settings.h"
#include "stage_config.h"
#include "wifi_manager.h"
#if LIGHTGW_HAS_ZIGBEE
#include "zigbee_manager.h"
#endif

static const char *TAG = "web";
static httpd_handle_t s_server;
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

static bool authorized(httpd_req_t *req)
{
    if (wifi_manager_is_provisioning() && !settings_get()->auth_configured) return true;
    size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (len < 7 || len > 200) goto denied;
    char *header = malloc(len + 1);
    if (!header) goto denied;
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, len + 1) != ESP_OK || strncmp(header, "Basic ", 6) != 0) {
        free(header);
        goto denied;
    }
    unsigned char decoded[128];
    size_t out_len = 0;
    int rc = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &out_len,
                                   (unsigned char *)header + 6, strlen(header + 6));
    free(header);
    if (rc != 0 || out_len >= sizeof(decoded)) goto denied;
    decoded[out_len] = '\0';
    char *colon = strchr((char *)decoded, ':');
    if (!colon) goto denied;
    *colon = '\0';
    if (settings_verify_admin((char *)decoded, colon + 1)) return true;
denied:
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32 Light Gateway\"");
    httpd_resp_sendstr(req, "Authentication required");
    return false;
}

static cJSON *receive_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 1024) return NULL;
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

static esp_err_t index_get(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
}

static esp_err_t status_get(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    char ip[16];
    wifi_manager_get_ip(ip, sizeof(ip));
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddStringToObject(root, "firmware", LIGHTGW_VERSION);
    cJSON_AddNumberToObject(root, "stage", LIGHTGW_STAGE);
    cJSON_AddBoolToObject(root, "provisioning", wifi_manager_is_provisioning());
    cJSON_AddStringToObject(root, "provision_ssid", wifi_manager_provision_ssid());
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_manager_is_connected());
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddNumberToObject(root, "wifi_rssi", wifi_manager_get_rssi());
    cJSON_AddNumberToObject(root, "uptime_seconds", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "minimum_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(root, "reset_reason", diagnostics_reset_reason());
    char last_error[160];
    diagnostics_get_last_error(last_error, sizeof(last_error));
    cJSON_AddStringToObject(root, "last_error", last_error);
    cJSON_AddBoolToObject(root, "time_synced", wifi_manager_time_is_synced());
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

    const app_settings_t *cfg = settings_get();
    cJSON *sched = cJSON_AddObjectToObject(root, "schedule");
    cJSON_AddBoolToObject(sched, "enabled", cfg->schedule_enabled);
    cJSON_AddNumberToObject(sched, "hour", cfg->schedule_hour);
    cJSON_AddNumberToObject(sched, "minute", cfg->schedule_minute);
    cJSON_AddNumberToObject(sched, "days", cfg->schedule_days);
    cJSON_AddStringToObject(sched, "timezone", cfg->timezone);

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
        for (int ch = 0; ch < 3; ++ch) cJSON_AddItemToArray(states, cJSON_CreateNumber(devices[i].state[ch]));
        cJSON *discovered = cJSON_AddArrayToObject(d, "discovered_endpoints");
        for (int ep = 0; ep < devices[i].discovered_endpoint_count; ++ep) {
            cJSON_AddItemToArray(discovered, cJSON_CreateNumber(devices[i].discovered_endpoints[ep]));
        }
        cJSON *onoff = cJSON_AddArrayToObject(d, "onoff_server_endpoints");
        for (int ep = 0; ep < devices[i].onoff_endpoint_count; ++ep) {
            cJSON_AddItemToArray(onoff, cJSON_CreateNumber(devices[i].onoff_endpoints[ep]));
        }
        cJSON_AddItemToArray(array, d);
    }
#endif
    char *text = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text);
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

static esp_err_t provision_post(httpd_req_t *req)
{
    if (!wifi_manager_is_provisioning()) return send_json_error(req, "403 Forbidden", "Provisioning is closed");
    cJSON *j = receive_json(req);
    cJSON *ssid = j ? cJSON_GetObjectItem(j, "ssid") : NULL;
    cJSON *pass = j ? cJSON_GetObjectItem(j, "password") : NULL;
    cJSON *user = j ? cJSON_GetObjectItem(j, "admin_user") : NULL;
    cJSON *admin = j ? cJSON_GetObjectItem(j, "admin_password") : NULL;
    if (!cJSON_IsString(ssid) || !cJSON_IsString(pass) || !cJSON_IsString(user) || !cJSON_IsString(admin)) {
        cJSON_Delete(j);
        return send_json_error(req, "400 Bad Request", "Missing fields");
    }
    esp_err_t err = settings_save_admin(user->valuestring, admin->valuestring);
    if (err == ESP_OK) err = settings_save_wifi(ssid->valuestring, pass->valuestring);
    cJSON_Delete(j);
    if (err != ESP_OK) return send_json_error(req, "400 Bad Request", "Invalid settings (admin password must be at least 12 characters)");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restarting\":true}");
    xTaskCreate(restart_task, "restart", 2048, NULL, 3, NULL);
    return ESP_OK;
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
    esp_err_t err = zigbee_manager_set_channel(addr->valueint, channel->valueint, cJSON_IsTrue(on));
    cJSON_Delete(j);
    return err == ESP_OK ? httpd_resp_sendstr(req, "{\"ok\":true}") : send_json_error(req, "404 Not Found", esp_err_to_name(err));
}

static esp_err_t all_off_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    esp_err_t err = zigbee_manager_all_off();
    return (err == ESP_OK || err == ESP_ERR_NOT_FOUND) ? httpd_resp_sendstr(req, "{\"ok\":true}") : send_json_error(req, "503 Service Unavailable", esp_err_to_name(err));
}
#endif

#if LIGHTGW_HAS_SCHEDULER
static esp_err_t schedule_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *j = receive_json(req);
    cJSON *enabled = j ? cJSON_GetObjectItem(j, "enabled") : NULL;
    cJSON *hour = j ? cJSON_GetObjectItem(j, "hour") : NULL;
    cJSON *minute = j ? cJSON_GetObjectItem(j, "minute") : NULL;
    cJSON *days = j ? cJSON_GetObjectItem(j, "days") : NULL;
    cJSON *tz = j ? cJSON_GetObjectItem(j, "timezone") : NULL;
    if (!cJSON_IsBool(enabled) || !cJSON_IsNumber(hour) || !cJSON_IsNumber(minute) || !cJSON_IsNumber(days) || !cJSON_IsString(tz)) {
        cJSON_Delete(j);
        return send_json_error(req, "400 Bad Request", "Invalid schedule");
    }
    esp_err_t err = settings_save_schedule(cJSON_IsTrue(enabled), hour->valueint, minute->valueint, days->valueint, tz->valuestring);
    cJSON_Delete(j);
    return err == ESP_OK ? httpd_resp_sendstr(req, "{\"ok\":true}") : send_json_error(req, "400 Bad Request", esp_err_to_name(err));
}
#endif

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;
    cfg.stack_size = 8192;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "HTTP server start");
    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/provision", .method = HTTP_POST, .handler = provision_post},
#if LIGHTGW_HAS_ZIGBEE
        {.uri = "/api/pair", .method = HTTP_POST, .handler = pair_post},
#endif
#if LIGHTGW_HAS_CONTROL
        {.uri = "/api/mapping", .method = HTTP_POST, .handler = mapping_post},
        {.uri = "/api/switch", .method = HTTP_POST, .handler = switch_post},
        {.uri = "/api/all-off", .method = HTTP_POST, .handler = all_off_post},
#endif
#if LIGHTGW_HAS_SCHEDULER
        {.uri = "/api/schedule", .method = HTTP_POST, .handler = schedule_post},
#endif
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &handlers[i]), TAG, "register URI");
    }
    return ESP_OK;
}
