#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "diagnostics.h"
#include "stage_config.h"
#if LIGHTGW_HAS_CONTROL
#include "device_control.h"
#endif
#if LIGHTGW_HAS_IR
#include "ir_manager.h"
#endif
#if LIGHTGW_HAS_SCHEDULER
#include "scheduler.h"
#endif
#include "settings.h"
#include "time_manager.h"
#include "web_server.h"
#include "wifi_manager.h"
#if LIGHTGW_HAS_ZIGBEE
#include "zigbee_manager.h"
#endif

static const char *TAG = "light_gateway";

void app_main(void)
{
    diagnostics_init();
    ESP_LOGI(TAG, "Firmware %s, Stage %d, reset=%s, free_heap=%u, min_free_heap=%u",
             LIGHTGW_VERSION, LIGHTGW_STAGE, diagnostics_reset_reason(),
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(wifi_manager_start());
    ESP_ERROR_CHECK(time_manager_start());
#if LIGHTGW_HAS_ZIGBEE
    ESP_ERROR_CHECK(zigbee_manager_start());
#endif
#if LIGHTGW_HAS_IR
    ESP_ERROR_CHECK(ir_manager_start());
#endif
#if LIGHTGW_HAS_CONTROL
    ESP_ERROR_CHECK(device_control_start());
#endif
#if LIGHTGW_HAS_SCHEDULER
    ESP_ERROR_CHECK(scheduler_start());
#endif
    ESP_ERROR_CHECK(web_server_start());
    diagnostics_record_memory_event("startup_complete");
    ESP_LOGI(TAG, "ESP32-C6 standalone light gateway Stage %d started", LIGHTGW_STAGE);
}
