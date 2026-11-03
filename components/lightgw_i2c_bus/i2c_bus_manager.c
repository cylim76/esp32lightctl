#include "i2c_bus_manager.h"

#include <stdbool.h>
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "i2c_bus";
static i2c_master_bus_handle_t s_bus;
static esp_err_t s_init_result = ESP_ERR_INVALID_STATE;
static bool s_init_attempted;

esp_err_t i2c_bus_manager_init(void)
{
    if (s_init_attempted) return s_init_result;
    s_init_attempted = true;
    i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_LIGHTGW_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_LIGHTGW_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    s_init_result = i2c_new_master_bus(&config, &s_bus);
    if (s_init_result == ESP_OK) {
        ESP_LOGI(TAG, "Core I2C bus ready: SDA GPIO%d, SCL GPIO%d, %d Hz",
                 CONFIG_LIGHTGW_I2C_SDA_GPIO, CONFIG_LIGHTGW_I2C_SCL_GPIO,
                 CONFIG_LIGHTGW_I2C_FREQUENCY_HZ);
    } else {
        ESP_LOGE(TAG, "Core I2C bus init failed: %s", esp_err_to_name(s_init_result));
    }
    return s_init_result;
}

esp_err_t i2c_bus_manager_get(i2c_master_bus_handle_t *bus)
{
    if (!bus) return ESP_ERR_INVALID_ARG;
    esp_err_t err = i2c_bus_manager_init();
    if (err != ESP_OK) return err;
    *bus = s_bus;
    return ESP_OK;
}
