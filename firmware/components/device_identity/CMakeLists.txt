idf_component_register(
    SRCS "device_identity.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES esp_common
    PRIV_REQUIRES airdap_frame esp_app_format esp_hw_support mbedtls
)
