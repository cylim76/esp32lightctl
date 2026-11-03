#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "scheduler.h"
#include "settings.h"
#include "web_server.h"
#include "wifi_manager.h"
#include "zigbee_manager.h"

static const char *TAG = "light_gateway";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(wifi_manager_start());
    ESP_ERROR_CHECK(web_server_start());
    ESP_ERROR_CHECK(zigbee_manager_start());
    ESP_ERROR_CHECK(scheduler_start());
    ESP_LOGI(TAG, "ESP32-C6 standalone light gateway started");
}
