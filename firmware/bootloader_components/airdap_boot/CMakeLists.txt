idf_component_register(
    SRCS "hooks.c"
    INCLUDE_DIRS "../../components/board/include"
    REQUIRES esp_hal_gpio soc
)
