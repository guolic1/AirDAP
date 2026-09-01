#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "airdap_wifi_credentials.h"

enum {
    CREDENTIALS_MAGIC_0 = 'A',
    CREDENTIALS_MAGIC_1 = 'W',
    CREDENTIALS_MAGIC_2 = 'F',
    CREDENTIALS_MAGIC_3 = '1',
    CREDENTIALS_VERSION = 1,
};

static bool credentials_are_valid(
    const airdap_wifi_credentials_t *credentials)
{
    return credentials != NULL &&
        credentials->ssid_length > 0U &&
        credentials->ssid_length <= AIRDAP_WIFI_SSID_MAX_LENGTH &&
        credentials->password_length <= AIRDAP_WIFI_PASSWORD_MAX_LENGTH;
}

bool airdap_wifi_credentials_encode(
    const airdap_wifi_credentials_t *credentials,
    uint8_t *output,
    size_t *inout_size)
{
    if (!credentials_are_valid(credentials) || output == NULL ||
        inout_size == NULL) {
        return false;
    }

    const size_t encoded_size = AIRDAP_WIFI_CREDENTIALS_HEADER_SIZE +
        credentials->ssid_length + credentials->password_length;
    if (*inout_size < encoded_size) {
        *inout_size = encoded_size;
        return false;
    }

    output[0] = CREDENTIALS_MAGIC_0;
    output[1] = CREDENTIALS_MAGIC_1;
    output[2] = CREDENTIALS_MAGIC_2;
    output[3] = CREDENTIALS_MAGIC_3;
    output[4] = CREDENTIALS_VERSION;
    output[5] = credentials->ssid_length;
    output[6] = credentials->password_length;
    output[7] = 0U;
    memcpy(
        &output[AIRDAP_WIFI_CREDENTIALS_HEADER_SIZE],
        credentials->ssid,
        credentials->ssid_length);
    memcpy(
        &output[AIRDAP_WIFI_CREDENTIALS_HEADER_SIZE +
            credentials->ssid_length],
        credentials->password,
        credentials->password_length);
    *inout_size = encoded_size;
    return true;
}

bool airdap_wifi_credentials_decode(
    const uint8_t *encoded,
    size_t encoded_size,
    airdap_wifi_credentials_t *credentials)
{
    if (credentials == NULL) {
        return false;
    }
    airdap_wifi_credentials_clear(credentials);
    if (encoded == NULL || encoded_size < AIRDAP_WIFI_CREDENTIALS_HEADER_SIZE ||
        encoded[0] != CREDENTIALS_MAGIC_0 ||
        encoded[1] != CREDENTIALS_MAGIC_1 ||
        encoded[2] != CREDENTIALS_MAGIC_2 ||
        encoded[3] != CREDENTIALS_MAGIC_3 ||
        encoded[4] != CREDENTIALS_VERSION || encoded[7] != 0U) {
        return false;
    }

    const uint8_t ssid_length = encoded[5];
    const uint8_t password_length = encoded[6];
    const size_t expected_size = AIRDAP_WIFI_CREDENTIALS_HEADER_SIZE +
        ssid_length + password_length;
    if (ssid_length == 0U || ssid_length > AIRDAP_WIFI_SSID_MAX_LENGTH ||
        password_length > AIRDAP_WIFI_PASSWORD_MAX_LENGTH ||
        encoded_size != expected_size) {
        return false;
    }

    credentials->ssid_length = ssid_length;
    credentials->password_length = password_length;
    memcpy(
        credentials->ssid,
        &encoded[AIRDAP_WIFI_CREDENTIALS_HEADER_SIZE],
        ssid_length);
    memcpy(
        credentials->password,
        &encoded[AIRDAP_WIFI_CREDENTIALS_HEADER_SIZE + ssid_length],
        password_length);
    return true;
}

void airdap_wifi_credentials_clear(
    airdap_wifi_credentials_t *credentials)
{
    if (credentials == NULL) {
        return;
    }
    volatile uint8_t *byte = (volatile uint8_t *) credentials;
    for (size_t index = 0U; index < sizeof(*credentials); ++index) {
        byte[index] = 0U;
    }
}
