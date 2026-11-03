set(app_sources
    "main.c"
    "settings.c"
    "wifi_manager.c"
    "rtc_manager.c"
    "time_manager.c"
    "web_server.c"
    "diagnostics.c"
    "storage_manager.c"
)
set(app_requires
    esp_coex esp_event esp_http_server esp_netif esp_system esp_timer esp_wifi espcoredump
    driver json lwip mbedtls mdns nvs_flash
)

if(LIGHTGW_STAGE GREATER_EQUAL 2)
    list(APPEND app_sources "zigbee_manager.c" "zigbee_state_cache.c")
endif()
if(LIGHTGW_STAGE GREATER_EQUAL 4)
    list(APPEND app_sources "scheduler.c" "scheduler_persistence.c" "device_control.c" "device_state.c")
endif()
if(LIGHTGW_STAGE GREATER_EQUAL 5)
    list(APPEND app_sources
        "ir_manager.c"
        "ir_receiver.c"
        "ir_transmitter.c"
        "ir_storage.c"
    )
endif()

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    EMBED_FILES "web/index.html"
    REQUIRES ${app_requires}
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE LIGHTGW_STAGE=${LIGHTGW_STAGE})
