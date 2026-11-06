set(app_sources
    "main.c"
    "settings.c"
    "wifi_manager.c"
    "rtc_manager.c"
    "time_manager.c"
    "web_server.c"
    "diagnostics.c"
    "storage_manager.c"
    "backup_integrity.c"
    "auth_password.c"
    "backup_restore.c"
    "extension_manager.c"
)
set(app_requires
    esp_coex esp_event esp_http_server esp_netif esp_system esp_timer esp_wifi espcoredump
    driver json lightgw_i2c_bus lwip mbedtls mdns nvs_flash
)

# Read the variant through an ESP-IDF build property. Component dependency
# discovery runs in an early expansion context where ordinary parent-scope
# CMake variables are empty, while build properties remain available.
idf_build_get_property(lightgw_build_variant LIGHTGW_VARIANT_NAME)

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
set(lightgw_bh1750_extension OFF)
set(lightgw_sht31_extension OFF)
set(lightgw_rainmaker_extension OFF)
if(lightgw_build_variant STREQUAL "CORE_SENSOR" OR lightgw_build_variant STREQUAL "FULL")
    set(lightgw_bh1750_extension ON)
    set(lightgw_sht31_extension ON)
endif()
if(lightgw_build_variant STREQUAL "CORE_CLOUD" OR lightgw_build_variant STREQUAL "FULL")
    set(lightgw_rainmaker_extension ON)
endif()

# Component dependencies are resolved during an early CMake expansion pass in
# which sdkconfig symbols are not guaranteed to be available. Derive the
# dependency graph from the cache-selected variant so an enabled extension can
# never be compiled without its component dependency.
if(lightgw_bh1750_extension)
    list(APPEND app_requires bh1750)
endif()
if(lightgw_sht31_extension)
    list(APPEND app_requires sht31)
endif()
if(lightgw_rainmaker_extension)
    list(APPEND app_requires rainmaker)
endif()

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    EMBED_FILES "web/index.html"
    REQUIRES ${app_requires}
)

if(LIGHTGW_VARIANT STREQUAL "STAGE")
    set(_lightgw_variant_build 0)
else()
    set(_lightgw_variant_build 1)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LIGHTGW_STAGE=${LIGHTGW_STAGE}
    LIGHTGW_VARIANT_BUILD=${_lightgw_variant_build}
)
