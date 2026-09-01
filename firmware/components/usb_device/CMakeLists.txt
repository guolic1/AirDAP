set(usb_device_sources
    "dap_stream.c"
    "usb_descriptors.c"
    "usb_device.c"
)
set(usb_device_private_requires
    config_store
    dap_ownership
    dap_protocol
    dap_service
    device_identity
    esp_system
    esp_tinyusb
    esp_timer
    freertos
    log
    mode_state
    swd_engine
    target_uart
    voltage_monitor
)

if(CONFIG_AIRDAP_DEBUG_SHELL)
    if(CONFIG_TINYUSB_VENDOR_COUNT LESS 2)
        message(FATAL_ERROR
            "CONFIG_AIRDAP_DEBUG_SHELL requires CONFIG_TINYUSB_VENDOR_COUNT=2")
    endif()
    list(APPEND usb_device_sources
        "debug_shell.c"
        "debug_shell_config_status.c"
        "debug_shell_identity.c"
        "debug_shell_input.c"
        "debug_shell_swd_probe.c"
        "debug_shell_tx_state.c"
    )
endif()

idf_component_register(
    SRCS ${usb_device_sources}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    PRIV_REQUIRES ${usb_device_private_requires}
)
