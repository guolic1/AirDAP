idf_component_register(
    SRCS "network_dap.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES esp_common
    PRIV_REQUIRES airdap_frame dap_service device_identity esp-tls esp_timer freertos log lwip mode_state network_auth network_control network_uart target_uart
)
