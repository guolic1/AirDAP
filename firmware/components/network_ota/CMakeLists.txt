idf_component_register(
    SRCS "network_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES airdap_frame network_auth
    PRIV_REQUIRES ota_manager log
)
