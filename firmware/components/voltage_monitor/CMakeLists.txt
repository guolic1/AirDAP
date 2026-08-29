idf_component_register(
    SRCS "voltage_monitor.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES board esp_adc
)
