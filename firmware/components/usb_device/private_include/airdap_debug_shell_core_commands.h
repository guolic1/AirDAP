#pragma once

#include <stdbool.h>

#include "airdap_debug_shell_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

bool airdap_debug_shell_register_core_commands(
    airdap_debug_shell_command_registry_t *registry);

int airdap_debug_shell_help_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context);

int airdap_debug_shell_wifi_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context);

int airdap_debug_shell_swd_idcode_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context);

int airdap_debug_shell_restart_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context);

#ifdef __cplusplus
}
#endif
