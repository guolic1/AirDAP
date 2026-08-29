idf_component_register(
    SRCS "usb_descriptors.c" "usb_device.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    PRIV_REQUIRES
        dap_protocol
        esp_hw_support
        esp_tinyusb
        freertos
        log
        target_uart
)
