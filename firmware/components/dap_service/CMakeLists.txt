idf_component_register(
    SRCS "dap_service.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES esp_common
    PRIV_REQUIRES dap_protocol esp_timer freertos
)
