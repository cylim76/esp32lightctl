idf_component_register(
    SRCS "rainmaker_extension.c" "rainmaker_control_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_rainmaker
    PRIV_REQUIRES esp_timer
)
