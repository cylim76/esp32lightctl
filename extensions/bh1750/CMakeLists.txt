idf_component_register(
    SRCS "bh1750_extension.c" "bh1750_protocol.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer lightgw_i2c_bus nvs_flash
)
