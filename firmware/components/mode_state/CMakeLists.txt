idf_component_register(
    SRCS "mode_state.c" "mode_storage.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES dap_ownership
    PRIV_REQUIRES nvs_flash
)
