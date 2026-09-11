idf_component_register(
    SRCS "network_control.c"
    INCLUDE_DIRS "include"
    REQUIRES airdap_frame network_auth
    PRIV_REQUIRES board mode_state dap_ownership
)
