idf_component_register(
    SRCS
        "main.c"
        "settings.c"
        "wifi_manager.c"
        "web_server.c"
        "zigbee_manager.c"
        "scheduler.c"
    INCLUDE_DIRS "."
    EMBED_FILES "web/index.html"
    REQUIRES
        esp_coex esp_event esp_http_server esp_netif esp_system esp_timer esp_wifi espcoredump
        json lwip mbedtls nvs_flash
)
