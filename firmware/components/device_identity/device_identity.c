#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_device_identity.h"
#include "airdap_device_identity_internal.h"
#include "airdap_frame.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "psa/crypto.h"

enum {
    SHA256_SIZE = 32,
};

_Static_assert(
    (unsigned int) AIRDAP_DEVICE_PROTOCOL_VERSION ==
        (unsigned int) AIRDAP_FRAME_PROTOCOL_VERSION,
    "device identity and AirDAP frame protocol versions must match");

static const uint8_t product_namespace[] = {'A', 'i', 'r', 'D', 'A', 'P'};
static airdap_device_identity_t device_identity;
static bool initialized;

esp_err_t airdap_device_identity_build(
    const uint8_t mac[AIRDAP_DEVICE_MAC_SIZE],
    const char *firmware_version,
    airdap_device_identity_t *identity)
{
    if (mac == NULL || firmware_version == NULL || identity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    airdap_device_identity_t candidate = {0};
    const int serial_length = snprintf(
        candidate.usb_serial,
        sizeof(candidate.usb_serial),
        "ADP-%02X%02X%02X%02X%02X%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (serial_length != AIRDAP_DEVICE_SERIAL_LENGTH) {
        return ESP_FAIL;
    }
    memcpy(candidate.device_id, candidate.usb_serial, sizeof(candidate.device_id));

    uint8_t hash_input[sizeof(product_namespace) + AIRDAP_DEVICE_MAC_SIZE];
    memcpy(hash_input, product_namespace, sizeof(product_namespace));
    memcpy(hash_input + sizeof(product_namespace), mac, AIRDAP_DEVICE_MAC_SIZE);
    uint8_t digest[SHA256_SIZE];
    size_t digest_length = 0U;
    const psa_status_t hash_status = psa_hash_compute(
        PSA_ALG_SHA_256,
        hash_input,
        sizeof(hash_input),
        digest,
        sizeof(digest),
        &digest_length);
    if (hash_status != PSA_SUCCESS || digest_length != sizeof(digest)) {
        return ESP_FAIL;
    }

    memcpy(candidate.uuid, digest, sizeof(candidate.uuid));
    candidate.firmware_version = firmware_version;
    candidate.protocol_version = AIRDAP_DEVICE_PROTOCOL_VERSION;
    candidate.capabilities =
        AIRDAP_CAPABILITY_SWD |
        AIRDAP_CAPABILITY_TARGET_UART |
        AIRDAP_CAPABILITY_TARGET_POWER |
        AIRDAP_CAPABILITY_TARGET_RESET |
        AIRDAP_CAPABILITY_USB_OTA;

    *identity = candidate;
    return ESP_OK;
}

esp_err_t airdap_device_identity_init(void)
{
    if (initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    uint8_t mac[AIRDAP_DEVICE_MAC_SIZE];
    const esp_err_t mac_error = esp_efuse_mac_get_default(mac);
    if (mac_error != ESP_OK) {
        return mac_error;
    }

    const esp_app_desc_t *app_description = esp_app_get_description();
    if (app_description == NULL) {
        return ESP_FAIL;
    }

    airdap_device_identity_t candidate;
    const esp_err_t identity_error = airdap_device_identity_build(
        mac,
        app_description->version,
        &candidate);
    if (identity_error != ESP_OK) {
        return identity_error;
    }

    device_identity = candidate;
    initialized = true;
    return ESP_OK;
}

const airdap_device_identity_t *airdap_device_identity_get(void)
{
    return initialized ? &device_identity : NULL;
}
