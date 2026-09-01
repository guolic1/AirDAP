#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airdap_wifi_manager.h"

enum {
    AIRDAP_WIFI_CREDENTIALS_HEADER_SIZE = 8,
    AIRDAP_WIFI_CREDENTIALS_ENCODED_MAX_SIZE =
        AIRDAP_WIFI_CREDENTIALS_HEADER_SIZE +
        AIRDAP_WIFI_SSID_MAX_LENGTH +
        AIRDAP_WIFI_PASSWORD_MAX_LENGTH,
};

bool airdap_wifi_credentials_encode(
    const airdap_wifi_credentials_t *credentials,
    uint8_t *output,
    size_t *inout_size);

bool airdap_wifi_credentials_decode(
    const uint8_t *encoded,
    size_t encoded_size,
    airdap_wifi_credentials_t *credentials);

void airdap_wifi_credentials_clear(
    airdap_wifi_credentials_t *credentials);
