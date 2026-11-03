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
    int last_yday = -1;
    int last_year = -1;
    char active_tz[65] = "";
    for (;;) {
        const app_settings_t *cfg = settings_get();
        if (strcmp(active_tz, cfg->timezone) != 0) {
            strlcpy(active_tz, cfg->timezone, sizeof(active_tz));
            setenv("TZ", active_tz, 1);
            tzset();
        }
        if (cfg->schedule_enabled && wifi_manager_is_connected() && wifi_manager_time_is_synced() &&
            zigbee_manager_network_ready()) {
            time_t now;
            struct tm local;
            time(&now);
            localtime_r(&now, &local);
            bool day_selected = (cfg->schedule_days & (1U << local.tm_wday)) != 0;
            bool already_ran = local.tm_yday == last_yday && local.tm_year == last_year;
            if (day_selected && !already_ran && local.tm_hour == cfg->schedule_hour && local.tm_min == cfg->schedule_minute) {
                esp_err_t err = zigbee_manager_all_off();
                if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
                    last_yday = local.tm_yday;
                    last_year = local.tm_year;
                    ESP_LOGI(TAG, "Daily all-off executed at %02d:%02d", local.tm_hour, local.tm_min);
                } else {
                    ESP_LOGE(TAG, "Daily all-off failed: %s", esp_err_to_name(err));
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
