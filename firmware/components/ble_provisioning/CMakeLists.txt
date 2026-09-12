idf_component_register(
    SRCS
        "ble_provisioning.c"
        "provisioning_button.c"
        "button_indicator.c"
        "button_device_commands.c"
        "button_simulation.c"
        "sec2_credentials.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES esp_common button_config
    PRIV_REQUIRES board config_store dap_ownership device_identity esp_event esp_timer esp_wifi freertos log mode_state network_auth network_provisioning protocomm wifi_manager
)
