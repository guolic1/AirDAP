#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "airdap_config_store.h"

enum {
    AIRDAP_DEBUG_SHELL_CONFIG_STATUS_OUTPUT_SIZE = 128,
};

bool airdap_debug_shell_config_status_format(
    const airdap_config_status_t *status,
    char *output,
    size_t output_size);
