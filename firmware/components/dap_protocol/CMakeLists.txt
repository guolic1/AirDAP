idf_component_register(
    SRCS "dap_backend.c" "dap_ota.c" "dap_protocol.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    PRIV_REQUIRES board esp_common esp_rom ota_manager swd_engine
)
