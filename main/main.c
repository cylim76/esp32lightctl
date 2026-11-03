#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "diagnostics.h"
#include "storage_manager.h"
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
    esp_err_t err = storage_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Storage degraded: %s. No data erased; automatic scheduling may be disabled.",
                 esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(wifi_manager_start());
    ESP_ERROR_CHECK(time_manager_start());
#if LIGHTGW_HAS_ZIGBEE
    ESP_ERROR_CHECK(zigbee_manager_start());
#endif
#if LIGHTGW_HAS_IR
    err = ir_manager_start();
    if (err != ESP_OK) {
        diagnostics_set_error(TAG, "IR subsystem disabled: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "IR subsystem disabled: %s; lighting and Web remain available", esp_err_to_name(err));
    }
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
