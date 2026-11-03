idf_component_register(
    SRCS "bh1750_extension.c"
    INCLUDE_DIRS "include"
    REQUIRES lightgw_i2c_bus
)
