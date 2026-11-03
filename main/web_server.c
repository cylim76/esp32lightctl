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
#if LIGHTGW_HAS_IR
#include "app_config.h"
#include "ir_manager.h"
#endif
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
    if (req->content_len <= 0 || req->content_len > 2048) return NULL;
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
    cJSON_AddStringToObject(root, "timezone", cfg->timezone);
    cJSON_AddNumberToObject(root, "max_schedules", APP_MAX_SCHEDULES);
    cJSON *schedules = cJSON_AddArrayToObject(root, "schedules");
    for (size_t i = 0; i < cfg->schedule_count && i < APP_MAX_SCHEDULES; ++i) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddBoolToObject(entry, "enabled", cfg->schedules[i].enabled != 0);
        cJSON_AddNumberToObject(entry, "hour", cfg->schedules[i].hour);
        cJSON_AddNumberToObject(entry, "minute", cfg->schedules[i].minute);
        cJSON_AddNumberToObject(entry, "days", cfg->schedules[i].days);
        cJSON_AddItemToArray(schedules, entry);
    }

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

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
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
    esp_err_t err = ir_manager_enqueue_send(mask->valueint, repeats->valueint, "manual");
    cJSON_Delete(j);
    return err == ESP_OK ? httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"IR queued; AC state remains unconfirmed\"}") :
                           send_json_error(req, "409 Conflict", esp_err_to_name(err));
}

static esp_err_t ir_schedule_post(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    cJSON *j = receive_json(req);
    cJSON *enabled = j ? cJSON_GetObjectItem(j, "enabled") : NULL;
    cJSON *same = j ? cJSON_GetObjectItem(j, "use_light_schedules") : NULL;
    cJSON *hour = j ? cJSON_GetObjectItem(j, "hour") : NULL;
    cJSON *minute = j ? cJSON_GetObjectItem(j, "minute") : NULL;
    cJSON *days = j ? cJSON_GetObjectItem(j, "days") : NULL;
    cJSON *mask = j ? cJSON_GetObjectItem(j, "slot_mask") : NULL;
    if (!cJSON_IsBool(enabled) || !cJSON_IsBool(same) || !cJSON_IsNumber(hour) || !cJSON_IsNumber(minute) ||
        !cJSON_IsNumber(days) || !cJSON_IsNumber(mask) || hour->valuedouble != hour->valueint ||
        minute->valuedouble != minute->valueint || days->valuedouble != days->valueint ||
        mask->valuedouble != mask->valueint || hour->valueint < 0 || hour->valueint > 23 ||
        minute->valueint < 0 || minute->valueint > 59 || days->valueint < 0 || days->valueint > 127 ||
        mask->valueint < 1 || mask->valueint > 3) {
        cJSON_Delete(j);
        return send_json_error(req, "400 Bad Request", "invalid IR schedule");
    }
    ir_schedule_t schedule = {
        .enabled = cJSON_IsTrue(enabled),
        .use_light_schedules = cJSON_IsTrue(same),
        .hour = hour->valueint,
        .minute = minute->valueint,
        .days = days->valueint,
        .slot_mask = mask->valueint,
    };
    esp_err_t err = ir_manager_save_schedule(&schedule);
    cJSON_Delete(j);
    return err == ESP_OK ? httpd_resp_sendstr(req, "{\"ok\":true}") :
                           send_json_error(req, "400 Bad Request", esp_err_to_name(err));
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
        cJSON *hour = cJSON_GetObjectItem(item, "hour");
        cJSON *minute = cJSON_GetObjectItem(item, "minute");
        cJSON *days = cJSON_GetObjectItem(item, "days");
        if (!cJSON_IsBool(enabled) || !cJSON_IsNumber(hour) || !cJSON_IsNumber(minute) ||
            !cJSON_IsNumber(days) || hour->valuedouble != hour->valueint ||
            minute->valuedouble != minute->valueint || days->valuedouble != days->valueint ||
            hour->valueint < 0 || hour->valueint > 23 || minute->valueint < 0 ||
            minute->valueint > 59 || days->valueint < 0 || days->valueint > 127) {
            cJSON_Delete(j);
            return send_json_error(req, "400 Bad Request", "Each schedule requires enabled/hour/minute/days");
        }
        entries[i] = (app_schedule_entry_t){cJSON_IsTrue(enabled), hour->valueint,
                                             minute->valueint, days->valueint};
    }
    esp_err_t err = settings_save_schedules(entries, count, tz->valuestring);
    cJSON_Delete(j);
    return err == ESP_OK ? httpd_resp_sendstr(req, "{\"ok\":true}") : send_json_error(req, "400 Bad Request", esp_err_to_name(err));
}
#endif

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 8192;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "HTTP server start");
    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/provision", .method = HTTP_POST, .handler = provision_post},
        {.uri = "/api/wifi/reconfigure", .method = HTTP_POST, .handler = wifi_reconfigure_post},
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
#if LIGHTGW_HAS_IR
        {.uri = "/api/ir/learn", .method = HTTP_POST, .handler = ir_learn_post},
        {.uri = "/api/ir/save", .method = HTTP_POST, .handler = ir_save_post},
        {.uri = "/api/ir/send", .method = HTTP_POST, .handler = ir_send_post},
        {.uri = "/api/ir/schedule", .method = HTTP_POST, .handler = ir_schedule_post},
#endif
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &handlers[i]), TAG, "register URI");
    }
    return ESP_OK;
}
