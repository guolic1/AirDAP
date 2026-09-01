idf_component_register(
    SRCS
        "wifi_credentials.c"
        "wifi_disconnect_reason.c"
        "wifi_manager.c"
        "wifi_state_machine.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES esp_common
    PRIV_REQUIRES config_store esp_event esp_netif esp_timer esp_wifi freertos log mode_state
)
