#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "airdap_sec2_credentials.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

static const char partition_name[] = "sec2_keys";
static const char credentials_namespace[] = "security2";
static const uint8_t fingerprint_domain[] = "AirDAP-Security2-v1";

static void clear_bytes(void *data, size_t size)
{
    volatile uint8_t *byte = (volatile uint8_t *) data;
    for (size_t index = 0U; index < size; ++index) {
        byte[index] = 0U;
    }
}

void airdap_sec2_credentials_clear(
    airdap_sec2_credentials_t *credentials)
{
    if (credentials == NULL) {
        return;
    }
    clear_bytes(credentials, sizeof(*credentials));
}

static esp_err_t read_exact_blob(
    nvs_handle_t handle,
    const char *key,
    uint8_t *output,
    size_t expected_size)
{
    size_t stored_size = 0U;
    esp_err_t error = nvs_get_blob(handle, key, NULL, &stored_size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (error != ESP_OK) {
        return error;
    }
    if (stored_size != expected_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    stored_size = expected_size;
    error = nvs_get_blob(handle, key, output, &stored_size);
    if (error != ESP_OK) {
        return error;
    }
    return stored_size == expected_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t make_fingerprint(airdap_sec2_credentials_t *credentials)
{
    uint8_t input[sizeof(fingerprint_domain) - 1U +
        AIRDAP_SEC2_SALT_SIZE + AIRDAP_SEC2_VERIFIER_SIZE];
    size_t offset = 0U;
    memcpy(input + offset, fingerprint_domain, sizeof(fingerprint_domain) - 1U);
    offset += sizeof(fingerprint_domain) - 1U;
    memcpy(input + offset, credentials->salt, sizeof(credentials->salt));
    offset += sizeof(credentials->salt);
    memcpy(input + offset, credentials->verifier, sizeof(credentials->verifier));

    uint8_t digest[AIRDAP_SEC2_FINGERPRINT_SIZE];
    size_t digest_length = 0U;
    const psa_status_t status = psa_hash_compute(
        PSA_ALG_SHA_256,
        input,
        sizeof(input),
        digest,
        sizeof(digest),
        &digest_length);
    clear_bytes(input, sizeof(input));
    if (status != PSA_SUCCESS || digest_length != sizeof(digest)) {
        clear_bytes(digest, sizeof(digest));
        return ESP_FAIL;
    }

    static const char hex[] = "0123456789ABCDEF";
    for (size_t index = 0U; index < sizeof(digest); ++index) {
        credentials->fingerprint_hex[index * 2U] = hex[digest[index] >> 4U];
        credentials->fingerprint_hex[index * 2U + 1U] =
            hex[digest[index] & 0x0FU];
    }
    credentials->fingerprint_hex[AIRDAP_SEC2_FINGERPRINT_HEX_LENGTH] = '\0';
    clear_bytes(digest, sizeof(digest));
    return ESP_OK;
}

esp_err_t airdap_sec2_credentials_load(
    airdap_sec2_credentials_t *credentials)
{
    if (credentials == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    airdap_sec2_credentials_clear(credentials);

    esp_err_t error = nvs_flash_init_partition(partition_name);
    if (error != ESP_OK) {
        return error;
    }

    nvs_handle_t handle = 0U;
    bool handle_open = false;
    error = nvs_open_from_partition(
        partition_name,
        credentials_namespace,
        NVS_READONLY,
        &handle);
    handle_open = error == ESP_OK;
    if (error == ESP_OK) {
        error = read_exact_blob(
            handle,
            "salt",
            credentials->salt,
            sizeof(credentials->salt));
    }
    if (error == ESP_OK) {
        error = read_exact_blob(
            handle,
            "verifier",
            credentials->verifier,
            sizeof(credentials->verifier));
    }
    if (error == ESP_OK) {
        credentials->salt_len = sizeof(credentials->salt);
        credentials->verifier_len = sizeof(credentials->verifier);
        error = make_fingerprint(credentials);
    }
    if (handle_open) {
        nvs_close(handle);
    }
    const esp_err_t deinit_error = nvs_flash_deinit_partition(partition_name);
    if (error == ESP_OK && deinit_error != ESP_OK) {
        error = deinit_error;
    }
    if (error != ESP_OK) {
        airdap_sec2_credentials_clear(credentials);
    }
    return error;
}
