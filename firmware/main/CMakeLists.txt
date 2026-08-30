idf_component_register(
    SRCS "app_main.c"
    PRIV_REQUIRES board device_identity esp_common log ota_manager swd_engine usb_device voltage_monitor
    INCLUDE_DIRS ""
)
