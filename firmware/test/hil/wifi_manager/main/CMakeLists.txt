idf_component_register(
    SRCS "wifi_manager_hil.c"
    REQUIRES config_store esp_common log mode_state wifi_manager
)
