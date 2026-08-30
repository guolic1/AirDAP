#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "airdap_debug_shell_config_status.h"
#include "esp_err.h"

bool airdap_debug_shell_config_status_format(
    const airdap_config_status_t *status,
    char *output,
    size_t output_size)
{
    if (status == NULL || output == NULL || output_size == 0U) {
        return false;
    }
    const int formatted = snprintf(
        output,
        output_size,
        "schema_version=%" PRIu32 "\nprovisioning_state=%s\n",
        status->schema_version,
        status->provisioned ? "provisioned" : "unprovisioned");
    return formatted >= 0 && (size_t) formatted < output_size;
}

int airdap_debug_shell_config_status_execute(
    const char *arguments,
    char *output,
    size_t output_size,
    airdap_debug_shell_config_status_style_t *style)
{
    if (arguments == NULL || output == NULL || output_size == 0U ||
        style == NULL) {
        return 1;
    }
    if (*arguments != '\0') {
        *style = AIRDAP_DEBUG_SHELL_CONFIG_STATUS_STYLE_YELLOW;
        (void) snprintf(output, output_size, "usage: config-status\n");
        return 1;
    }

    airdap_config_status_t status;
    const esp_err_t error = airdap_config_store_get_status(&status);
    if (error != ESP_OK) {
        *style = AIRDAP_DEBUG_SHELL_CONFIG_STATUS_STYLE_RED;
        (void) snprintf(
            output,
            output_size,
            "config-status: read failed: %s\n",
            esp_err_to_name(error));
        return 1;
    }

    *style = AIRDAP_DEBUG_SHELL_CONFIG_STATUS_STYLE_GREEN;
    if (airdap_debug_shell_config_status_format(
            &status,
            output,
            output_size)) {
        return 0;
    }
    *style = AIRDAP_DEBUG_SHELL_CONFIG_STATUS_STYLE_RED;
    (void) snprintf(output, output_size, "config-status: formatting failed\n");
    return 1;
}
