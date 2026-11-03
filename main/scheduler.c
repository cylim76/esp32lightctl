#include "scheduler.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include "wifi_manager.h"
#include "zigbee_manager.h"

static const char *TAG = "scheduler";

static void scheduler_task(void *arg)
{
    (void)arg;
    int last_yday[APP_MAX_SCHEDULES];
    int last_year[APP_MAX_SCHEDULES];
    uint32_t last_signature[APP_MAX_SCHEDULES];
    for (size_t i = 0; i < APP_MAX_SCHEDULES; ++i) {
        last_yday[i] = -1;
        last_year[i] = -1;
        last_signature[i] = UINT32_MAX;
    }
    char active_tz[65] = "";
    for (;;) {
        const app_settings_t *cfg = settings_get();
        if (strcmp(active_tz, cfg->timezone) != 0) {
            strlcpy(active_tz, cfg->timezone, sizeof(active_tz));
            setenv("TZ", active_tz, 1);
            tzset();
        }
        if (cfg->schedule_count && wifi_manager_is_connected() && wifi_manager_time_is_synced() &&
            zigbee_manager_network_ready()) {
            time_t now;
            struct tm local;
            time(&now);
            localtime_r(&now, &local);
            for (size_t i = 0; i < cfg->schedule_count && i < APP_MAX_SCHEDULES; ++i) {
                const app_schedule_entry_t *entry = &cfg->schedules[i];
                uint32_t signature = ((uint32_t)entry->enabled << 24) | ((uint32_t)entry->days << 16) |
                                     ((uint32_t)entry->hour << 8) | entry->minute;
                if (signature != last_signature[i]) {
                    last_signature[i] = signature;
                    last_yday[i] = -1;
                    last_year[i] = -1;
                }
                bool day_selected = (entry->days & (1U << local.tm_wday)) != 0;
                bool already_ran = local.tm_yday == last_yday[i] && local.tm_year == last_year[i];
                if (!entry->enabled || !day_selected || already_ran ||
                    local.tm_hour != entry->hour || local.tm_min != entry->minute) continue;
                esp_err_t err = zigbee_manager_all_off();
                if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
                    last_yday[i] = local.tm_yday;
                    last_year[i] = local.tm_year;
                    ESP_LOGI(TAG, "All-off schedule %u executed at %02d:%02d", (unsigned)(i + 1),
                             local.tm_hour, local.tm_min);
                } else {
                    ESP_LOGE(TAG, "All-off schedule %u failed: %s", (unsigned)(i + 1), esp_err_to_name(err));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

esp_err_t scheduler_start(void)
{
    return xTaskCreate(scheduler_task, "scheduler", 4096, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
