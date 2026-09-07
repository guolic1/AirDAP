#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

enum {
    AIRDAP_NETWORK_AUTH_PSK_SIZE = 32,
    AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE = 32,
    AIRDAP_NETWORK_AUTH_PAIR_REQUEST_VERSION = 1,
    AIRDAP_NETWORK_AUTH_PAIR_REQUEST_SIZE =
        1 + AIRDAP_NETWORK_AUTH_PSK_SIZE,
};

typedef enum {
    AIRDAP_NETWORK_AUTH_OK = 0,
    AIRDAP_NETWORK_AUTH_INVALID_ARGUMENT,
    AIRDAP_NETWORK_AUTH_INVALID_STATE,
    AIRDAP_NETWORK_AUTH_UNSUPPORTED_VERSION,
    AIRDAP_NETWORK_AUTH_NO_CREDENTIAL,
    AIRDAP_NETWORK_AUTH_STORAGE_FAILED,
} airdap_network_auth_result_t;

airdap_network_auth_result_t airdap_network_auth_pair(
    const uint8_t *request,
    size_t request_size,
    uint8_t fingerprint[AIRDAP_NETWORK_AUTH_FINGERPRINT_SIZE]);

esp_err_t airdap_network_auth_set_pairing_window_active(bool active);
