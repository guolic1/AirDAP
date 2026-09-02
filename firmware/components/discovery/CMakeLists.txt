idf_component_register(
    SRCS "discovery.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES device_identity esp_event esp_netif freertos log mdns mode_state
)
