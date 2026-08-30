idf_component_register(
    SRCS "ota_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES app_update esp_app_format esp_system
)
