#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_provisioning_button.h"
#include "airdap_sec2_credentials.h"
#include "nvs.h"
#include "psa/crypto.h"

enum {
    FAKE_HANDLE = 7,
    SALT_SIZE = 16,
    VERIFIER_SIZE = 384,
};

static uint8_t stored_salt[SALT_SIZE];
static uint8_t stored_verifier[VERIFIER_SIZE];
static esp_err_t partition_init_result;
static esp_err_t open_result;
static bool partition_initialized;
static bool handle_open;

esp_err_t nvs_flash_init_partition(const char *partition_name)
{
    assert(strcmp(partition_name, "sec2_keys") == 0);
    if (partition_init_result == ESP_OK) {
        partition_initialized = true;
    }
    return partition_init_result;
}

esp_err_t nvs_flash_deinit_partition(const char *partition_name)
{
    assert(strcmp(partition_name, "sec2_keys") == 0);
    partition_initialized = false;
    return ESP_OK;
}

esp_err_t nvs_open_from_partition(
    const char *partition_name,
    const char *namespace_name,
    nvs_open_mode_t open_mode,
    nvs_handle_t *handle)
{
    assert(partition_initialized);
    assert(strcmp(partition_name, "sec2_keys") == 0);
    assert(strcmp(namespace_name, "security2") == 0);
    assert(open_mode == NVS_READONLY);
    assert(handle != NULL);
    if (open_result != ESP_OK) {
        return open_result;
    }
    *handle = FAKE_HANDLE;
    handle_open = true;
    return ESP_OK;
}

esp_err_t nvs_get_blob(
    nvs_handle_t handle,
    const char *key,
    void *output,
    size_t *length)
{
    assert(handle == FAKE_HANDLE && handle_open && length != NULL);
    const uint8_t *source = NULL;
    size_t source_length = 0U;
    if (strcmp(key, "salt") == 0) {
        source = stored_salt;
        source_length = sizeof(stored_salt);
    } else if (strcmp(key, "verifier") == 0) {
        source = stored_verifier;
        source_length = sizeof(stored_verifier);
    } else {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (output == NULL) {
        *length = source_length;
        return ESP_OK;
    }
    if (*length < source_length) {
        *length = source_length;
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(output, source, source_length);
    *length = source_length;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == FAKE_HANDLE && handle_open);
    handle_open = false;
}

psa_status_t psa_hash_compute(
    psa_algorithm_t algorithm,
    const uint8_t *input,
    size_t input_length,
    uint8_t *hash,
    size_t hash_size,
    size_t *hash_length)
{
    assert(algorithm == PSA_ALG_SHA_256);
    assert(input != NULL && input_length > VERIFIER_SIZE);
    assert(hash != NULL && hash_size == AIRDAP_SEC2_FINGERPRINT_SIZE);
    assert(hash_length != NULL);
    for (size_t index = 0U; index < hash_size; ++index) {
        hash[index] = (uint8_t) (input[index % input_length] ^ index);
    }
    *hash_length = hash_size;
    return PSA_SUCCESS;
}

static void reset_fakes(void)
{
    for (size_t index = 0U; index < sizeof(stored_salt); ++index) {
        stored_salt[index] = (uint8_t) index;
    }
    for (size_t index = 0U; index < sizeof(stored_verifier); ++index) {
        stored_verifier[index] = (uint8_t) (index + 1U);
    }
    partition_init_result = ESP_OK;
    open_result = ESP_OK;
    partition_initialized = false;
    handle_open = false;
}

static void test_button_thresholds_are_one_shot(void)
{
    airdap_provisioning_button_t button;
    airdap_provisioning_button_init(&button);

    for (unsigned tick = 0U; tick < 29U; ++tick) {
        assert(airdap_provisioning_button_step(&button, true, 100U) ==
            AIRDAP_PROVISIONING_BUTTON_NONE);
    }
    assert(airdap_provisioning_button_step(&button, true, 100U) ==
        AIRDAP_PROVISIONING_BUTTON_TOGGLE);
    for (unsigned tick = 0U; tick < 69U; ++tick) {
        assert(airdap_provisioning_button_step(&button, true, 100U) ==
            AIRDAP_PROVISIONING_BUTTON_NONE);
    }
    assert(airdap_provisioning_button_step(&button, true, 100U) ==
        AIRDAP_PROVISIONING_BUTTON_CLEAR);
    assert(airdap_provisioning_button_step(&button, true, 5000U) ==
        AIRDAP_PROVISIONING_BUTTON_NONE);
    assert(airdap_provisioning_button_step(&button, false, 100U) ==
        AIRDAP_PROVISIONING_BUTTON_RELEASED);
    assert(airdap_provisioning_button_step(&button, false, 100U) ==
        AIRDAP_PROVISIONING_BUTTON_NONE);
}

static void test_security2_credentials_load_and_clear(void)
{
    reset_fakes();
    airdap_sec2_credentials_t credentials;
    assert(airdap_sec2_credentials_load(&credentials) == ESP_OK);
    assert(credentials.salt_len == sizeof(stored_salt));
    assert(credentials.verifier_len == sizeof(stored_verifier));
    assert(memcmp(credentials.salt, stored_salt, sizeof(stored_salt)) == 0);
    assert(memcmp(
        credentials.verifier,
        stored_verifier,
        sizeof(stored_verifier)) == 0);
    assert(credentials.fingerprint_hex[AIRDAP_SEC2_FINGERPRINT_HEX_LENGTH] == '\0');
    assert(!handle_open && !partition_initialized);

    airdap_sec2_credentials_clear(&credentials);
    const uint8_t *bytes = (const uint8_t *) &credentials;
    for (size_t index = 0U; index < sizeof(credentials); ++index) {
        assert(bytes[index] == 0U);
    }
}

static void test_security2_load_failures_do_not_leave_material(void)
{
    reset_fakes();
    airdap_sec2_credentials_t credentials;
    memset(&credentials, 0xA5, sizeof(credentials));
    open_result = ESP_FAIL;
    assert(airdap_sec2_credentials_load(&credentials) == ESP_FAIL);
    const uint8_t *bytes = (const uint8_t *) &credentials;
    for (size_t index = 0U; index < sizeof(credentials); ++index) {
        assert(bytes[index] == 0U);
    }
    assert(!partition_initialized && !handle_open);

    reset_fakes();
    memset(&credentials, 0xA5, sizeof(credentials));
    partition_init_result = ESP_FAIL;
    assert(airdap_sec2_credentials_load(&credentials) == ESP_FAIL);
    for (size_t index = 0U; index < sizeof(credentials); ++index) {
        assert(bytes[index] == 0U);
    }
}

int main(void)
{
    test_button_thresholds_are_one_shot();
    test_security2_credentials_load_and_clear();
    test_security2_load_failures_do_not_leave_material();
    puts("BLE provisioning core tests passed");
    return 0;
}
