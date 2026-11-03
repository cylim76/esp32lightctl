idf_component_register(
    SRCS
        "phase_a_tests.c"
        "../../../main/zigbee_state_cache.c"
        "../../../main/device_state.c"
        "../../../main/scheduler_persistence.c"
    INCLUDE_DIRS "../../../main"
    REQUIRES nvs_flash
)
