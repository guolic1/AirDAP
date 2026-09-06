#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "airdap_debug_shell_config_status.h"

static void assert_has_no_sensitive_field(const char *output)
{
    static const char *const forbidden[] = {
        "password",
        "pop",
        "psk",
        "private_key",
        "wifi_credentials",
        "pairing_record",
        "auth_material",
    };
    for (size_t index = 0U;
         index < sizeof(forbidden) / sizeof(forbidden[0]);
         ++index) {
        assert(strstr(output, forbidden[index]) == NULL);
    }
}

static void test_formats_only_safe_status_fields(void)
{
    const airdap_config_status_t unprovisioned = {
        .schema_version = 1U,
        .provisioned = false,
    };
    const airdap_config_status_t provisioned = {
        .schema_version = 7U,
        .provisioned = true,
    };
    char output[AIRDAP_DEBUG_SHELL_CONFIG_STATUS_OUTPUT_SIZE];

    assert(airdap_debug_shell_config_status_format(
        &unprovisioned,
        output,
        sizeof(output)));
    assert(strcmp(
        output,
        "schema_version=1\nprovisioning_state=unprovisioned\n") == 0);
    assert_has_no_sensitive_field(output);

    assert(airdap_debug_shell_config_status_format(
        &provisioned,
        output,
        sizeof(output)));
    assert(strcmp(
        output,
        "schema_version=7\nprovisioning_state=provisioned\n") == 0);
    assert_has_no_sensitive_field(output);
}

static void test_rejects_invalid_arguments_and_short_output(void)
{
    const airdap_config_status_t status = {
        .schema_version = 1U,
        .provisioned = false,
    };
    char output[16];

    assert(!airdap_debug_shell_config_status_format(
        NULL,
        output,
        sizeof(output)));
    assert(!airdap_debug_shell_config_status_format(
        &status,
        NULL,
        sizeof(output)));
    assert(!airdap_debug_shell_config_status_format(&status, output, 0U));
    assert(!airdap_debug_shell_config_status_format(
        &status,
        output,
        sizeof(output)));
}

int main(void)
{
    test_formats_only_safe_status_fields();
    test_rejects_invalid_arguments_and_short_output();

    puts("Debug shell config status formatting tests passed");
    return 0;
}
