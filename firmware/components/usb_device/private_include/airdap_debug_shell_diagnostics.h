#pragma once

#include <stdbool.h>

#include "airdap_debug_shell_commands.h"

bool airdap_debug_shell_register_diagnostic_commands(
    airdap_debug_shell_command_registry_t *registry);
