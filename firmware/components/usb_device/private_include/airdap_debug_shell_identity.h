#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "airdap_device_identity.h"

enum {
    AIRDAP_DEBUG_SHELL_IDENTITY_OUTPUT_SIZE = 256,
};

bool airdap_debug_shell_identity_format(
    const airdap_device_identity_t *identity,
    char *output,
    size_t output_size);
