#pragma once

#include <stdint.h>

#include "airdap_device_identity.h"

esp_err_t airdap_device_identity_build(
    const uint8_t mac[AIRDAP_DEVICE_MAC_SIZE],
    const char *firmware_version,
    airdap_device_identity_t *identity);
