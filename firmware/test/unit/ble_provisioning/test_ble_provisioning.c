#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_provisioning_button.h"
#include "airdap_sec2_credentials.h"

static const uint8_t expected_salt[AIRDAP_SEC2_SALT_SIZE] = {
    0x03, 0x6e, 0xe0, 0xc7, 0xbc, 0xb9, 0xed, 0xa8,
    0x4c, 0x9e, 0xac, 0x97, 0xd9, 0x3d, 0xec, 0xf4,
};

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

static void test_public_security2_credentials_load_and_clear(void)
{
    assert(strcmp(AIRDAP_SEC2_USERNAME, "wifiprov") == 0);
    assert(strcmp(AIRDAP_SEC2_POP, "abcd1234") == 0);

    airdap_sec2_credentials_t credentials;
    assert(airdap_sec2_credentials_load(&credentials) == ESP_OK);
    assert(credentials.salt_len == AIRDAP_SEC2_SALT_SIZE);
    assert(credentials.verifier_len == AIRDAP_SEC2_VERIFIER_SIZE);
    assert(memcmp(credentials.salt, expected_salt, sizeof(expected_salt)) == 0);
    assert(strcmp(
        credentials.fingerprint_hex,
        "C5D2AA01B4DDA9A67CBE111D61B9F0CBBD3A9F7A7935E85E570A881C7EE03080") == 0);
    assert(credentials.verifier[0] == 0x7c);
    assert(credentials.verifier[AIRDAP_SEC2_VERIFIER_SIZE - 1U] == 0xba);

    airdap_sec2_credentials_clear(&credentials);
    const uint8_t *bytes = (const uint8_t *) &credentials;
    for (size_t index = 0U; index < sizeof(credentials); ++index) {
        assert(bytes[index] == 0U);
    }
}

int main(void)
{
    test_button_thresholds_are_one_shot();
    test_public_security2_credentials_load_and_clear();
    puts("BLE provisioning core tests passed");
    return 0;
}
