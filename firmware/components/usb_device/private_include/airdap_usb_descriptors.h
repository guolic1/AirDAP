#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tusb.h"

enum {
    AIRDAP_USB_DAP_INTERFACE = 0,
    AIRDAP_USB_CDC_CONTROL_INTERFACE = 1,
    AIRDAP_USB_CDC_DATA_INTERFACE = 2,
    AIRDAP_USB_INTERFACE_COUNT = 3,
};

void airdap_usb_descriptors_set_serial(const char *serial_number);
const tusb_desc_device_t *airdap_usb_device_descriptor(void);
const uint8_t *airdap_usb_configuration_descriptor(void);
const char **airdap_usb_string_descriptors(void);
size_t airdap_usb_string_descriptor_count(void);
