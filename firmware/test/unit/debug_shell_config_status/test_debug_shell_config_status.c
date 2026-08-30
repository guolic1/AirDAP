#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "airdap_debug_shell_commands.h"
#include "airdap_debug_shell_config_status.h"

static esp_err_t status_result;
static airdap_config_status_t stored_status;
static unsigned status_calls;

esp_err_t airdap_config_store_get_status(airdap_config_status_t *status)
{
    ++status_calls;
    if (status_result == ESP_OK) {
        *status = stored_status;
    }
    return status_result;
}

const char *esp_err_to_name(esp_err_t error)
{
    return error == ESP_FAIL ? "ESP_FAIL" : "ESP_UNKNOWN";
}

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

static void test_command_registry_has_only_supported_commands(void)
{
    static const char *const command_names[] = {
#define AIRDAP_DEBUG_SHELL_COMMAND_NAME(name_, help_, handler_) name_,
        AIRDAP_DEBUG_SHELL_COMMAND_LIST(AIRDAP_DEBUG_SHELL_COMMAND_NAME)
#undef AIRDAP_DEBUG_SHELL_COMMAND_NAME
    };
    static const char *const expected_names[] = {
        "help",
        "identity",
        "config-status",
        "status",
        "swd-idcode",
        "restart",
    };
    assert(sizeof(command_names) == sizeof(expected_names));
    for (size_t index = 0U;
         index < sizeof(expected_names) / sizeof(expected_names[0]);
         ++index) {
        assert(strcmp(command_names[index], expected_names[index]) == 0);
        assert(strcmp(command_names[index], "version") != 0);
    }
}

static void test_command_boundary_reports_safe_status_and_errors(void)
{
    char output[AIRDAP_DEBUG_SHELL_CONFIG_STATUS_OUTPUT_SIZE];
    airdap_debug_shell_config_status_style_t style;

    status_result = ESP_OK;
    stored_status.schema_version = 3U;
    stored_status.provisioned = true;
    status_calls = 0U;
    assert(airdap_debug_shell_config_status_execute(
        "",
        output,
        sizeof(output),
        &style) == 0);
    assert(style == AIRDAP_DEBUG_SHELL_CONFIG_STATUS_STYLE_GREEN);
    assert(strcmp(
        output,
        "schema_version=3\nprovisioning_state=provisioned\n") == 0);
    assert(status_calls == 1U);
    assert_has_no_sensitive_field(output);

    assert(airdap_debug_shell_config_status_execute(
        "extra",
        output,
        sizeof(output),
        &style) == 1);
    assert(style == AIRDAP_DEBUG_SHELL_CONFIG_STATUS_STYLE_YELLOW);
    assert(strcmp(output, "usage: config-status\n") == 0);
    assert(status_calls == 1U);

    status_result = ESP_FAIL;
    assert(airdap_debug_shell_config_status_execute(
        "",
        output,
        sizeof(output),
        &style) == 1);
    assert(style == AIRDAP_DEBUG_SHELL_CONFIG_STATUS_STYLE_RED);
    assert(strcmp(output, "config-status: read failed: ESP_FAIL\n") == 0);
    assert(status_calls == 2U);
    assert_has_no_sensitive_field(output);
}

int main(void)
{
    test_formats_only_safe_status_fields();
    test_rejects_invalid_arguments_and_short_output();
    test_command_registry_has_only_supported_commands();
    test_command_boundary_reports_safe_status_and_errors();

    puts("Debug shell config-status tests passed");
    return 0;
}
