idf_component_register(
    SRCS "app_main.c"
    PRIV_REQUIRES ble_provisioning board config_store device_identity discovery esp_common esp_event log mode_state ota_manager swd_engine usb_device voltage_monitor wifi_manager
    INCLUDE_DIRS ""
)
