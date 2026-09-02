idf_component_register(
    SRCS
        "ble_provisioning.c"
        "provisioning_button.c"
        "sec2_credentials.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES esp_common
    PRIV_REQUIRES board config_store device_identity esp_event esp_timer esp_wifi freertos log mode_state nvs_flash network_provisioning protocomm wifi_manager mbedtls
)
