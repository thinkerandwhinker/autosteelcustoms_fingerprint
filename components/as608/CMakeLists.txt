idf_component_register(
    SRCS "as608.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_driver_gpio
    PRIV_REQUIRES freertos log
)