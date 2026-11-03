idf_component_register(
    SRCS "sht31_extension.c"
    INCLUDE_DIRS "include"
    REQUIRES lightgw_i2c_bus
)
