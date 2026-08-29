idf_component_register(
    SRCS "target_uart.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES board esp_driver_uart
)
