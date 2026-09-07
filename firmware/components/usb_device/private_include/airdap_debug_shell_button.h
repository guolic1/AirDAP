#pragma once

#include "airdap_debug_shell_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

int airdap_debug_shell_button_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context);

#ifdef __cplusplus
}
#endif
