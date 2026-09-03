#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define AIRDAP_SEC2_USERNAME "wifiprov"
#define AIRDAP_SEC2_POP "abcd1234"

enum {
    AIRDAP_SEC2_SALT_SIZE = 16,
    AIRDAP_SEC2_VERIFIER_SIZE = 384,
    AIRDAP_SEC2_FINGERPRINT_SIZE = 32,
    AIRDAP_SEC2_FINGERPRINT_HEX_LENGTH = AIRDAP_SEC2_FINGERPRINT_SIZE * 2,
};

typedef struct {
    uint8_t salt[AIRDAP_SEC2_SALT_SIZE];
    uint8_t verifier[AIRDAP_SEC2_VERIFIER_SIZE];
    uint16_t salt_len;
    uint16_t verifier_len;
    char fingerprint_hex[AIRDAP_SEC2_FINGERPRINT_HEX_LENGTH + 1];
} airdap_sec2_credentials_t;

esp_err_t airdap_sec2_credentials_load(
    airdap_sec2_credentials_t *credentials);
void airdap_sec2_credentials_clear(
    airdap_sec2_credentials_t *credentials);
