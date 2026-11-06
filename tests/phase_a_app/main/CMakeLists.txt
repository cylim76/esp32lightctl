idf_component_register(
    SRCS
        "phase_a_tests.c"
        "../../../main/zigbee_state_cache.c"
        "../../../main/device_state.c"
        "../../../main/scheduler_persistence.c"
        "../../../main/backup_integrity.c"
        "../../../main/auth_password.c"
        "../../../extensions/bh1750/bh1750_protocol.c"
        "../../../extensions/sht31/sht31_protocol.c"
        "../../../extensions/rainmaker/rainmaker_control_policy.c"
    INCLUDE_DIRS
        "../../../main"
        "../../../extensions/bh1750/include"
        "../../../extensions/sht31/include"
        "../../../extensions/rainmaker/include"
    REQUIRES mbedtls nvs_flash
)
