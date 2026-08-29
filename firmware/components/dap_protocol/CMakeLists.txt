idf_component_register(
    SRCS "dap_backend.c" "dap_protocol.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES board esp_common esp_rom swd_engine
)
