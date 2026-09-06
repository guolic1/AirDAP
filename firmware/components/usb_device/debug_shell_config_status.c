#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "airdap_debug_shell_config_status.h"

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
