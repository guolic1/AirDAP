#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_device_identity.h"
#include "airdap_device_identity_internal.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "psa/crypto.h"

enum {
    SHA256_SIZE = 32,
    HASH_INPUT_SIZE = 12,
};

typedef struct {
    uint8_t input[HASH_INPUT_SIZE];
    uint8_t digest[SHA256_SIZE];
} hash_vector_t;

static const hash_vector_t hash_vectors[] = {
    {
        .input = {'A', 'i', 'r', 'D', 'A', 'P', 0x00, 0x11, 0x22, 0x33, 0x44, 0x55},
        .digest = {
            0x9B, 0x08, 0x73, 0x85, 0x83, 0x42, 0x21, 0x8E,
            0x83, 0xAD, 0xDA, 0x3D, 0x2A, 0x8C, 0xC9, 0x58,
            0xB4, 0x67, 0xC1, 0xAE, 0xA2, 0x33, 0x17, 0x18,
            0x32, 0x34, 0xED, 0x0B, 0xC5, 0x55, 0xA3, 0xF5,
        },
    },
    {
        .input = {'A', 'i', 'r', 'D', 'A', 'P', 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
        .digest = {
            0x7B, 0x72, 0xB7, 0xF0, 0x4C, 0x8A, 0xFC, 0xA1,
            0x3D, 0x22, 0x25, 0x78, 0x3A, 0xAB, 0x79, 0xD0,
            0x0D, 0xB4, 0xE2, 0x1D, 0x45, 0x53, 0x03, 0x22,
            0x77, 0x07, 0xCC, 0x0B, 0x14, 0xFD, 0x0F, 0xF8,
        },
    },
};

static uint8_t runtime_mac[AIRDAP_DEVICE_MAC_SIZE] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
};
static esp_err_t mac_result = ESP_OK;
static psa_status_t crypto_result = PSA_SUCCESS;
static psa_status_t hash_result = PSA_SUCCESS;
static const esp_app_desc_t app_description = {
    .version = "1.2.3-test",
};

const esp_app_desc_t *esp_app_get_description(void)
{
    return &app_description;
}

esp_err_t esp_efuse_mac_get_default(uint8_t *mac)
{
    assert(mac != NULL);
    if (mac_result == ESP_OK) {
        memcpy(mac, runtime_mac, sizeof(runtime_mac));
    }
    return mac_result;
}

psa_status_t psa_crypto_init(void)
{
    return crypto_result;
}

psa_status_t psa_hash_compute(
    psa_algorithm_t alg,
    const uint8_t *input,
    size_t input_length,
    uint8_t *hash,
    size_t hash_size,
    size_t *hash_length)
{
    assert(alg == PSA_ALG_SHA_256);
    assert(input != NULL);
    assert(input_length == HASH_INPUT_SIZE);
    assert(hash != NULL);
    assert(hash_size == SHA256_SIZE);
    assert(hash_length != NULL);
    if (hash_result != PSA_SUCCESS) {
        return hash_result;
    }

    for (size_t index = 0U; index < sizeof(hash_vectors) / sizeof(hash_vectors[0]); ++index) {
        if (memcmp(input, hash_vectors[index].input, HASH_INPUT_SIZE) == 0) {
            memcpy(hash, hash_vectors[index].digest, SHA256_SIZE);
            *hash_length = SHA256_SIZE;
            return PSA_SUCCESS;
        }
    }

    assert(false);
    return PSA_ERROR_GENERIC_ERROR;
}

static void assert_current_capabilities(uint32_t capabilities)
{
    const uint32_t expected =
        AIRDAP_CAPABILITY_SWD |
        AIRDAP_CAPABILITY_TARGET_UART |
        AIRDAP_CAPABILITY_TARGET_POWER |
        AIRDAP_CAPABILITY_TARGET_RESET |
        AIRDAP_CAPABILITY_USB_OTA;
    assert(capabilities == expected);
}

static void test_builds_stable_identity_from_full_mac(void)
{
    static const uint8_t mac[AIRDAP_DEVICE_MAC_SIZE] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
    };
    static const uint8_t expected_uuid[AIRDAP_DEVICE_UUID_SIZE] = {
        0x9B, 0x08, 0x73, 0x85, 0x83, 0x42, 0x21, 0x8E,
        0x83, 0xAD, 0xDA, 0x3D, 0x2A, 0x8C, 0xC9, 0x58,
    };
    airdap_device_identity_t first = {0};
    airdap_device_identity_t second = {0};

    assert(airdap_device_identity_build(mac, "V1.2.3", &first) == ESP_OK);
    assert(airdap_device_identity_build(mac, "V1.2.3", &second) == ESP_OK);
    assert(strcmp(first.usb_serial, "ADP-001122334455") == 0);
    assert(strcmp(first.device_id, first.usb_serial) == 0);
    assert(memcmp(first.uuid, expected_uuid, sizeof(expected_uuid)) == 0);
    assert(strcmp(first.usb_serial, second.usb_serial) == 0);
    assert(strcmp(first.device_id, second.device_id) == 0);
    assert(memcmp(first.uuid, second.uuid, AIRDAP_DEVICE_UUID_SIZE) == 0);
    assert(strcmp(first.firmware_version, "V1.2.3") == 0);
    assert(strcmp(first.firmware_version, second.firmware_version) == 0);
    assert(first.protocol_version == AIRDAP_DEVICE_PROTOCOL_VERSION);
    assert(first.protocol_version == second.protocol_version);
    assert_current_capabilities(first.capabilities);
    assert(first.capabilities == second.capabilities);
}

static void test_different_mac_changes_all_stable_identifiers(void)
{
    static const uint8_t first_mac[AIRDAP_DEVICE_MAC_SIZE] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
    };
    static const uint8_t second_mac[AIRDAP_DEVICE_MAC_SIZE] = {
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
    };
    airdap_device_identity_t first = {0};
    airdap_device_identity_t second = {0};

    assert(airdap_device_identity_build(first_mac, "version", &first) == ESP_OK);
    assert(airdap_device_identity_build(second_mac, "version", &second) == ESP_OK);
    assert(strcmp(first.usb_serial, second.usb_serial) != 0);
    assert(strcmp(first.device_id, second.device_id) != 0);
    assert(memcmp(first.uuid, second.uuid, AIRDAP_DEVICE_UUID_SIZE) != 0);
}

static void test_rejects_invalid_input_and_hash_failures(void)
{
    static const uint8_t mac[AIRDAP_DEVICE_MAC_SIZE] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
    };
    airdap_device_identity_t identity = {0};

    assert(airdap_device_identity_build(NULL, "version", &identity) == ESP_ERR_INVALID_ARG);
    assert(airdap_device_identity_build(mac, NULL, &identity) == ESP_ERR_INVALID_ARG);
    assert(airdap_device_identity_build(mac, "version", NULL) == ESP_ERR_INVALID_ARG);

    hash_result = PSA_ERROR_GENERIC_ERROR;
    assert(airdap_device_identity_build(mac, "version", &identity) == ESP_FAIL);
    hash_result = PSA_SUCCESS;
}

static void test_runtime_initialization_publishes_only_complete_identity(void)
{
    assert(airdap_device_identity_get() == NULL);

    crypto_result = PSA_ERROR_GENERIC_ERROR;
    assert(airdap_device_identity_init() == ESP_FAIL);
    assert(airdap_device_identity_get() == NULL);

    crypto_result = PSA_SUCCESS;
    mac_result = ESP_FAIL;
    assert(airdap_device_identity_init() == ESP_FAIL);
    assert(airdap_device_identity_get() == NULL);

    mac_result = ESP_OK;
    assert(airdap_device_identity_init() == ESP_OK);
    const airdap_device_identity_t *identity = airdap_device_identity_get();
    assert(identity != NULL);
    assert(strcmp(identity->usb_serial, "ADP-001122334455") == 0);
    assert(strcmp(identity->firmware_version, app_description.version) == 0);
    assert(airdap_device_identity_init() == ESP_ERR_INVALID_STATE);
}

int main(void)
{
    test_builds_stable_identity_from_full_mac();
    test_different_mac_changes_all_stable_identifiers();
    test_rejects_invalid_input_and_hash_failures();
    test_runtime_initialization_publishes_only_complete_identity();

    puts("device identity tests passed");
    return 0;
}
