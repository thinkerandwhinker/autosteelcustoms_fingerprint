idf_component_register(
    SRCS "lcd1602_i2c.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_common
    PRIV_REQUIRES freertos esp_rom log
)
