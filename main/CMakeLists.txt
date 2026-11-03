set(app_sources
    "main.c"
    "settings.c"
    "wifi_manager.c"
    "web_server.c"
    "diagnostics.c"
)
set(app_requires
    esp_coex esp_event esp_http_server esp_netif esp_system esp_timer esp_wifi espcoredump
    json lwip mbedtls mdns nvs_flash
)

if(LIGHTGW_STAGE GREATER_EQUAL 2)
    list(APPEND app_sources "zigbee_manager.c")
endif()
if(LIGHTGW_STAGE GREATER_EQUAL 4)
    list(APPEND app_sources "scheduler.c")
endif()
if(LIGHTGW_STAGE GREATER_EQUAL 5)
    list(APPEND app_sources
        "ir_manager.c"
        "ir_receiver.c"
        "ir_transmitter.c"
        "ir_storage.c"
    )
    list(APPEND app_requires driver)
endif()

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    EMBED_FILES "web/index.html"
    REQUIRES ${app_requires}
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE LIGHTGW_STAGE=${LIGHTGW_STAGE})
