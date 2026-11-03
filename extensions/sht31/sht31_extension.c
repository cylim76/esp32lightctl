#include "sht31_extension.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "i2c_bus_manager.h"

#define SHT31_ADDRESS_LOW  0x44
#define SHT31_ADDRESS_HIGH 0x45

static const char *TAG = "sht31_ext";
static bool s_present;

esp_err_t sht31_extension_start(void)
{
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_bus_manager_get(&bus);
    if (err != ESP_OK) return err;
    s_present = i2c_master_probe(bus, SHT31_ADDRESS_LOW, 50) == ESP_OK ||
                i2c_master_probe(bus, SHT31_ADDRESS_HIGH, 50) == ESP_OK;
    ESP_LOGI(TAG, "Experimental probe: %s; sampling not implemented",
             s_present ? "present" : "absent");
    return ESP_OK;
}

bool sht31_extension_is_present(void)
{
    return s_present;
}
