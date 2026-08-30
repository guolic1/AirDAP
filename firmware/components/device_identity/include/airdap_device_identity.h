#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AIRDAP_DEVICE_MAC_SIZE = 6,
    AIRDAP_DEVICE_SERIAL_LENGTH = 16,
    AIRDAP_DEVICE_SERIAL_SIZE = AIRDAP_DEVICE_SERIAL_LENGTH + 1,
    AIRDAP_DEVICE_UUID_SIZE = 16,
    AIRDAP_DEVICE_PROTOCOL_VERSION = 1,

    AIRDAP_CAPABILITY_SWD = 1U << 0,
    AIRDAP_CAPABILITY_TARGET_UART = 1U << 1,
    AIRDAP_CAPABILITY_TARGET_POWER = 1U << 2,
    AIRDAP_CAPABILITY_TARGET_RESET = 1U << 3,
    AIRDAP_CAPABILITY_USB_OTA = 1U << 4,
};

typedef struct {
    char usb_serial[AIRDAP_DEVICE_SERIAL_SIZE];
    char device_id[AIRDAP_DEVICE_SERIAL_SIZE];
    uint8_t uuid[AIRDAP_DEVICE_UUID_SIZE];
    const char *firmware_version;
    uint8_t protocol_version;
    uint32_t capabilities;
} airdap_device_identity_t;

esp_err_t airdap_device_identity_init(void);
const airdap_device_identity_t *airdap_device_identity_get(void);

#ifdef __cplusplus
}
#endif
