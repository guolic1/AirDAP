idf_component_register(
    SRCS "dap_backend.c" "dap_ota.c" "dap_protocol.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES dap_ownership esp_common
    PRIV_REQUIRES board esp_rom mode_state ota_manager swd_engine
)
