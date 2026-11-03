set(app_sources
    "main.c"
    "settings.c"
    "wifi_manager.c"
    "web_server.c"
    "diagnostics.c"
)

if(LIGHTGW_STAGE GREATER_EQUAL 2)
    list(APPEND app_sources "zigbee_manager.c")
endif()
if(LIGHTGW_STAGE GREATER_EQUAL 4)
    list(APPEND app_sources "scheduler.c")
endif()

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    EMBED_FILES "web/index.html"
    REQUIRES
        esp_coex esp_event esp_http_server esp_netif esp_system esp_timer esp_wifi espcoredump
        json lwip mbedtls mdns nvs_flash
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE LIGHTGW_STAGE=${LIGHTGW_STAGE})
