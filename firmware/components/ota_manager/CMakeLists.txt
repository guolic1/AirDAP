idf_component_register(
    SRCS "ota_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES app_update dap_ownership device_identity esp_system mode_state
)
