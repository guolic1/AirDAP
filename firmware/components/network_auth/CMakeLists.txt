idf_component_register(
    SRCS "network_auth.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES config_store device_identity esp-tls esp_timer freertos mbedtls vfs
)
