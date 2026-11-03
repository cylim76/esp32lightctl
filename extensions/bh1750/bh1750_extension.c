#include "bh1750_extension.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "i2c_bus_manager.h"

#define BH1750_ADDRESS_LOW  0x23
#define BH1750_ADDRESS_HIGH 0x5C

static const char *TAG = "bh1750_ext";
static bool s_present;

esp_err_t bh1750_extension_start(void)
{
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_bus_manager_get(&bus);
    if (err != ESP_OK) return err;
    s_present = i2c_master_probe(bus, BH1750_ADDRESS_LOW, 50) == ESP_OK ||
                i2c_master_probe(bus, BH1750_ADDRESS_HIGH, 50) == ESP_OK;
    ESP_LOGI(TAG, "Experimental probe: %s; sampling not implemented",
             s_present ? "present" : "absent");
    return ESP_OK;
}

bool bh1750_extension_is_present(void)
{
    return s_present;
}
