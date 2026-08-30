idf_component_register(
    SRCS "config_store.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES esp_common
    PRIV_REQUIRES freertos nvs_flash
)
