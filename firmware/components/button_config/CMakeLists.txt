idf_component_register(SRCS "button_config.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES freertos nvs_flash)
