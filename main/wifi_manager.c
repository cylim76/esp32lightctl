#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_config.h"
#include "diagnostics.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mdns.h"
#include "settings.h"

#define WIFI_CONNECTED_BIT BIT0
#define TIME_SYNCED_BIT     BIT1

static const char *TAG = "wifi";
static EventGroupHandle_t s_events;
static char s_ip[16] = "0.0.0.0";
static char s_ap_ssid[33];
static char s_ap_password[17];
static bool s_provisioning;
static bool s_sntp_started;
static bool s_mdns_started;
static unsigned s_retry;
static esp_timer_handle_t s_retry_timer;
static esp_netif_t *s_sta_netif;

static esp_err_t start_mdns_once(void)
{
    if (s_mdns_started) return ESP_OK;
    const char *hostname = settings_get()->device_name;
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        diagnostics_set_error(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = mdns_hostname_set(hostname);
    if (err == ESP_OK) err = mdns_instance_name_set(hostname);
    if (err == ESP_OK) err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) {
        mdns_free();
        diagnostics_set_error(TAG, "mDNS start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_mdns_started = true;
    ESP_LOGI(TAG, "mDNS started: http://%s.local", hostname);
    return ESP_OK;
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void time_sync_cb(struct timeval *tv)
{
    (void)tv;
    xEventGroupSetBits(s_events, TIME_SYNCED_BIT);
    ESP_LOGI(TAG, "NTP time synchronized");
}

static void start_sntp_once(void)
{
    if (s_sntp_started) return;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
        ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.cloudflare.com"));
    config.sync_cb = time_sync_cb;
    if (esp_netif_sntp_init(&config) == ESP_OK) s_sntp_started = true;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | TIME_SYNCED_BIT);
        strlcpy(s_ip, "0.0.0.0", sizeof(s_ip));
        unsigned backoff_ms = 1000U << (s_retry < 5 ? s_retry : 5);
        ++s_retry;
        ESP_LOGW(TAG, "Wi-Fi disconnected reason=%u; retry %u in %u ms",
                 event ? event->reason : 0, s_retry, backoff_ms);
        if (s_mdns_started) ESP_LOGI(TAG, "mDNS remains registered and will follow the STA reconnect/IP change");
        diagnostics_set_error(TAG, "disconnected reason=%u", event ? event->reason : 0);
        if (!esp_timer_is_active(s_retry_timer)) esp_timer_start_once(s_retry_timer, (uint64_t)backoff_ms * 1000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected, IP: %s (DHCP)", s_ip);
        start_mdns_once();
        start_sntp_once();
    }
}

static esp_err_t start_provision_ap(void)
{
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "read MAC");
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), APP_PROVISION_AP_PREFIX "%02X%02X%02X", mac[3], mac[4], mac[5]);
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    for (size_t i = 0; i < 16; ++i) s_ap_password[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    s_ap_password[16] = '\0';
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, s_ap_password, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set AP mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "set AP config");
    s_provisioning = true;
    ESP_LOGW(TAG, "Provisioning AP '%s' started; one-time password: %s", s_ap_ssid, s_ap_password);
    return ESP_OK;
}

static esp_err_t start_sta(const app_settings_t *cfg)
{
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, cfg->wifi_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, cfg->wifi_password, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "set STA config");
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(s_sta_netif, cfg->device_name), TAG, "set DHCP hostname");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_MIN_MODEM), TAG, "set modem sleep");
    s_provisioning = false;
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    if (!s_sta_netif) return ESP_ERR_NO_MEM;

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi init");
    const esp_timer_create_args_t retry_args = {.callback = retry_timer_cb, .name = "wifi_retry"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&retry_args, &s_retry_timer), TAG, "retry timer");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL), TAG, "Wi-Fi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL), TAG, "IP handler");

    const app_settings_t *cfg = settings_get();
    ESP_RETURN_ON_ERROR(cfg->wifi_configured ? start_sta(cfg) : start_provision_ap(), TAG, "Wi-Fi configuration");
    return esp_wifi_start();
}

bool wifi_manager_is_connected(void)
{
    return s_events && (xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT);
}

bool wifi_manager_is_provisioning(void) { return s_provisioning; }

bool wifi_manager_time_is_synced(void)
{
    return s_events && (xEventGroupGetBits(s_events) & TIME_SYNCED_BIT);
}

void wifi_manager_get_ip(char *out, size_t out_len)
{
    if (out && out_len) strlcpy(out, s_ip, out_len);
}

const char *wifi_manager_provision_ssid(void) { return s_ap_ssid; }

void wifi_manager_get_mac(char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        strlcpy(out, "unknown", out_len);
    }
}

bool wifi_manager_mdns_active(void)
{
    return s_mdns_started && wifi_manager_is_connected();
}

const char *wifi_manager_mdns_hostname(void)
{
    return settings_get()->device_name;
}

int wifi_manager_get_rssi(void)
{
    if (!wifi_manager_is_connected()) return 0;
    wifi_ap_record_t ap = {0};
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}
