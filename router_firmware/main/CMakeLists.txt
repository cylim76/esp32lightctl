idf_component_register(
    SRCS "router_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_app_format esp_system esp_timer espcoredump nvs_flash espressif__esp-zigbee-lib
)
