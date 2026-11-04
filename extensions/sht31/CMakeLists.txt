idf_component_register(
    SRCS "sht31_extension.c" "sht31_protocol.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer lightgw_i2c_bus nvs_flash
)
