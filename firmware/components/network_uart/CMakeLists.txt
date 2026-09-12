idf_component_register(
    SRCS "network_uart.c"
    INCLUDE_DIRS "include"
    REQUIRES airdap_frame network_auth target_uart
)
