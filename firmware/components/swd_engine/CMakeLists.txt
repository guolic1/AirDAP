idf_component_register(
    SRCS
        "swd_engine.c"
        "swd_protocol.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES esp_common
    PRIV_REQUIRES board esp_driver_gpio esp_driver_spi esp_rom
)
