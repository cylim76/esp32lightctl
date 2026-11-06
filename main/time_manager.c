#include "time_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "app_config.h"
#include "diagnostics.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "rtc_manager.h"
#include "settings.h"
#include "wifi_manager.h"

typedef enum { NTP_REQUEST_AUTO = 1, NTP_REQUEST_MANUAL = 2 } ntp_request_t;

static const char *TAG = "time_manager";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static time_manager_status_t s_status;
static QueueHandle_t s_requests;
static int s_last_daily_key = -1;
static time_t s_last_ntp_attempt;

bool time_manager_epoch_valid(time_t value)
{
    return (int64_t)value >= APP_TIME_MIN_VALID_EPOCH && (int64_t)value <= APP_TIME_MAX_VALID_EPOCH;
}

const char *time_manager_source_name(time_source_t source)
{
    switch (source) {
    case TIME_SOURCE_NTP: return "NTP";
    case TIME_SOURCE_RTC: return "RTC";
    case TIME_SOURCE_MANUAL: return "MANUAL";
    case TIME_SOURCE_SYSTEM: return "SYSTEM";
    default: return "INVALID";
    }
}

static void set_error(const char *text)
{
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_status.last_error, text, sizeof(s_status.last_error));
    portEXIT_CRITICAL(&s_lock);
    diagnostics_set_error(TAG, "%s", text);
}

static void refresh_rtc_status(void)
{
    rtc_manager_status_t rtc;
    rtc_manager_get_status(&rtc);
    portENTER_CRITICAL(&s_lock);
    s_status.rtc_initialized = rtc.initialized;
    s_status.rtc_online = rtc.online;
    s_status.rtc_osf = rtc.osf;
    s_status.rtc_status_register = rtc.status_register;
    portEXIT_CRITICAL(&s_lock);
}

static void accept_time(time_t value, time_source_t source)
{
    struct timeval tv = {.tv_sec = value};
    settimeofday(&tv, NULL);
    portENTER_CRITICAL(&s_lock);
    s_status.valid = true;
    s_status.source = source;
    s_status.now_utc = value;
    s_status.last_error[0] = '\0';
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Time VALID source=%s UTC=%lld timezone=%s", time_manager_source_name(source),
             (long long)value, settings_get()->timezone);
}

static time_t next_daily_ntp(time_t now)
{
    if (!time_manager_epoch_valid(now)) return 0;
    struct tm local;
    localtime_r(&now, &local);
    local.tm_hour = APP_NTP_DAILY_HOUR_LOCAL;
    local.tm_min = 0;
    local.tm_sec = 0;
    time_t next = mktime(&local);
    if (next <= now) {
        local.tm_mday++;
        next = mktime(&local);
    }
    return next;
}

static esp_err_t one_ntp_attempt(void)
{
    time_t previous;
    time(&previous);
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
        ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.cloudflare.com"));
    config.start = true;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) return err;
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(APP_NTP_WAIT_SECONDS * 1000));
    esp_netif_sntp_deinit();
    if (err != ESP_OK) return err;
    time_t now;
    time(&now);
    if (time_manager_epoch_valid(now)) return ESP_OK;
    if (time_manager_epoch_valid(previous)) {
        struct timeval restore = {.tv_sec = previous};
        settimeofday(&restore, NULL);
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static void perform_ntp_sync(ntp_request_t request)
{
    if (!wifi_manager_is_connected()) {
        portENTER_CRITICAL(&s_lock);
        strlcpy(s_status.last_ntp_result, "skipped: Wi-Fi disconnected", sizeof(s_status.last_ntp_result));
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_status.ntp_in_progress = true;
    strlcpy(s_status.last_ntp_result, "synchronizing", sizeof(s_status.last_ntp_result));
    portEXIT_CRITICAL(&s_lock);
    time(&s_last_ntp_attempt);
    esp_err_t err = ESP_FAIL;
    for (unsigned attempt = 0; attempt < APP_NTP_RETRY_COUNT && wifi_manager_is_connected(); ++attempt) {
        err = one_ntp_attempt();
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "NTP attempt %u/%u failed: %s", attempt + 1, APP_NTP_RETRY_COUNT, esp_err_to_name(err));
        if (attempt + 1 < APP_NTP_RETRY_COUNT) vTaskDelay(pdMS_TO_TICKS((5U << attempt) * 1000));
    }
    if (err == ESP_OK) {
        time_t now;
        time(&now);
        time_t before;
        portENTER_CRITICAL(&s_lock);
        before = s_status.now_utc;
        portEXIT_CRITICAL(&s_lock);
        accept_time(now, TIME_SOURCE_NTP);
        rtc_manager_status_t rtc;
        rtc_manager_get_status(&rtc);
        esp_err_t rtc_err = rtc.online ? rtc_manager_write_utc(now) : ESP_ERR_NOT_FOUND;
        refresh_rtc_status();
        time_t next_sync = next_daily_ntp(now);
        portENTER_CRITICAL(&s_lock);
        s_status.last_ntp_sync = now;
        snprintf(s_status.last_ntp_result, sizeof(s_status.last_ntp_result),
                 "success%s; delta=%llds", rtc_err == ESP_OK ? "; RTC updated" : "; RTC update failed",
                 (long long)(before ? now - before : 0));
        s_status.next_ntp_sync = next_sync;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "NTP sync success request=%s RTC=%s", request == NTP_REQUEST_MANUAL ? "manual" : "auto",
                 esp_err_to_name(rtc_err));
    } else {
        bool previously_valid;
        portENTER_CRITICAL(&s_lock);
        previously_valid = s_status.valid;
        snprintf(s_status.last_ntp_result, sizeof(s_status.last_ntp_result), "failed: %s", esp_err_to_name(err));
        portEXIT_CRITICAL(&s_lock);
        set_error(previously_valid
                      ? "NTP synchronization failed; previously valid local time remains valid"
                      : "NTP synchronization failed; system time remains invalid");
    }
    portENTER_CRITICAL(&s_lock);
    s_status.ntp_in_progress = false;
    portEXIT_CRITICAL(&s_lock);
}

static void time_task(void *arg)
{
    (void)arg;
    bool was_connected = false;
    for (;;) {
        time_t now;
        time(&now);
        portENTER_CRITICAL(&s_lock);
        if (s_status.valid && time_manager_epoch_valid(now)) s_status.now_utc = now;
        bool valid = s_status.valid;
        portEXIT_CRITICAL(&s_lock);
        bool connected = wifi_manager_is_connected();
        if (connected && !was_connected && (!s_last_ntp_attempt || now - s_last_ntp_attempt >= 300)) {
            ntp_request_t request = NTP_REQUEST_AUTO;
            if (!s_status.ntp_in_progress && uxQueueMessagesWaiting(s_requests) == 0) xQueueSend(s_requests, &request, 0);
        }
        was_connected = connected;
        if (valid) {
            struct tm local;
            localtime_r(&now, &local);
            int date_key = (local.tm_year + 1900) * 1000 + local.tm_yday;
            if (local.tm_hour == APP_NTP_DAILY_HOUR_LOCAL && local.tm_min == 0 && date_key != s_last_daily_key) {
                ntp_request_t request = NTP_REQUEST_AUTO;
                if (!s_status.ntp_in_progress && uxQueueMessagesWaiting(s_requests) == 0 &&
                    xQueueSend(s_requests, &request, 0) == pdTRUE) s_last_daily_key = date_key;
            }
            time_t next_sync = next_daily_ntp(now);
            portENTER_CRITICAL(&s_lock);
            s_status.next_ntp_sync = next_sync;
            portEXIT_CRITICAL(&s_lock);
        }
        ntp_request_t request;
        if (xQueueReceive(s_requests, &request, pdMS_TO_TICKS(1000)) == pdTRUE) perform_ntp_sync(request);
    }
}

esp_err_t time_manager_start(void)
{
    memset(&s_status, 0, sizeof(s_status));
    strlcpy(s_status.timezone, settings_get()->timezone, sizeof(s_status.timezone));
    setenv("TZ", s_status.timezone, 1);
    tzset();
    strlcpy(s_status.last_ntp_result, "not attempted", sizeof(s_status.last_ntp_result));
    esp_err_t rtc_init = rtc_manager_init();
    refresh_rtc_status();
    if (rtc_init == ESP_OK) {
        time_t rtc_time;
        bool osf;
        esp_err_t err = rtc_manager_read_utc(&rtc_time, &osf);
        refresh_rtc_status();
        if (err == ESP_OK && !osf && time_manager_epoch_valid(rtc_time)) accept_time(rtc_time, TIME_SOURCE_RTC);
        else if (err == ESP_OK && osf) set_error("DS3231 OSF is set; RTC time not trusted");
        else set_error("DS3231 time invalid or outside configured range");
    } else {
        set_error("DS3231 unavailable; waiting for NTP or administrator time");
    }
    s_requests = xQueueCreate(2, sizeof(ntp_request_t));
    if (!s_requests) return ESP_ERR_NO_MEM;
    return xTaskCreate(time_task, "time_manager", 6144, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool time_manager_is_valid(void)
{
    portENTER_CRITICAL(&s_lock);
    bool valid = s_status.valid;
    portEXIT_CRITICAL(&s_lock);
    return valid;
}

void time_manager_get_status(time_manager_status_t *status)
{
    if (!status) return;
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t time_manager_request_ntp(void)
{
    if (!s_requests) return ESP_ERR_INVALID_STATE;
    if (!wifi_manager_is_connected()) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    bool busy = s_status.ntp_in_progress;
    portEXIT_CRITICAL(&s_lock);
    if (busy || uxQueueMessagesWaiting(s_requests)) return ESP_ERR_INVALID_STATE;
    ntp_request_t request = NTP_REQUEST_MANUAL;
    return xQueueSend(s_requests, &request, 0) == pdTRUE ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t time_manager_set_local(int year, int month, int day, int hour, int minute, int second)
{
    portENTER_CRITICAL(&s_lock);
    bool ntp_busy = s_status.ntp_in_progress;
    portEXIT_CRITICAL(&s_lock);
    if (ntp_busy || (s_requests && uxQueueMessagesWaiting(s_requests))) return ESP_ERR_INVALID_STATE;
    if (year < 2026 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return ESP_ERR_INVALID_ARG;
    struct tm local = {
        .tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day,
        .tm_hour = hour, .tm_min = minute, .tm_sec = second, .tm_isdst = -1,
    };
    time_t value = mktime(&local);
    struct tm roundtrip;
    localtime_r(&value, &roundtrip);
    if (roundtrip.tm_year != year - 1900 || roundtrip.tm_mon != month - 1 || roundtrip.tm_mday != day ||
        roundtrip.tm_hour != hour || roundtrip.tm_min != minute || roundtrip.tm_sec != second ||
        !time_manager_epoch_valid(value)) return ESP_ERR_INVALID_ARG;
    accept_time(value, TIME_SOURCE_MANUAL);
    rtc_manager_status_t rtc;
    rtc_manager_get_status(&rtc);
    esp_err_t rtc_err = rtc.online ? rtc_manager_write_utc(value) : ESP_ERR_NOT_FOUND;
    refresh_rtc_status();
    if (rtc_err != ESP_OK) set_error("ESP32 time set manually; DS3231 write failed");
    ESP_LOGW(TAG, "Administrator set local time %04d-%02d-%02d %02d:%02d:%02d CST; RTC=%s",
             year, month, day, hour, minute, second, esp_err_to_name(rtc_err));
    return ESP_OK;
}
