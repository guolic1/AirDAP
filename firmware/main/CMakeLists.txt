idf_component_register(
    SRCS "app_main.c"
    PRIV_REQUIRES board config_store device_identity esp_common log mode_state ota_manager swd_engine usb_device voltage_monitor wifi_manager
    INCLUDE_DIRS ""
)
