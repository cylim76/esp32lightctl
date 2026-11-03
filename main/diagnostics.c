#include "diagnostics.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;
static char s_last_error[160] = "none";
static const char *s_reset_reason = "unknown";

void diagnostics_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: s_reset_reason = "power_on"; break;
    case ESP_RST_SW: s_reset_reason = "software"; break;
    case ESP_RST_PANIC: s_reset_reason = "panic"; break;
    case ESP_RST_INT_WDT: s_reset_reason = "interrupt_watchdog"; break;
    case ESP_RST_TASK_WDT: s_reset_reason = "task_watchdog"; break;
    case ESP_RST_WDT: s_reset_reason = "watchdog"; break;
    case ESP_RST_BROWNOUT: s_reset_reason = "brownout"; break;
    case ESP_RST_DEEPSLEEP: s_reset_reason = "deep_sleep"; break;
    default: s_reset_reason = "other"; break;
    }
}

void diagnostics_set_error(const char *source, const char *format, ...)
{
    char detail[112];
    va_list args;
    va_start(args, format);
    vsnprintf(detail, sizeof(detail), format, args);
    va_end(args);
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_last_error, sizeof(s_last_error), "%s: %s", source ? source : "app", detail);
    if (s_lock) xSemaphoreGive(s_lock);
    ESP_LOGE(source ? source : "diagnostics", "%s", detail);
}

void diagnostics_get_last_error(char *out, size_t out_len)
{
    if (!out || !out_len) return;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out, s_last_error, out_len);
    if (s_lock) xSemaphoreGive(s_lock);
}

const char *diagnostics_reset_reason(void)
{
    return s_reset_reason;
}
