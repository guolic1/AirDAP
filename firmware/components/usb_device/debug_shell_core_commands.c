#include <stddef.h>

#include "airdap_debug_shell_button.h"
#include "airdap_debug_shell_core_commands.h"

static const airdap_debug_shell_command_t core_commands[] = {
    {
        .name = "help",
        .usage = "help [command]",
        .summary = "List commands or explain one command",
        .details =
            "Without an argument, lists registered commands. With a command "
            "name, prints its usage and full description.",
        .handler = airdap_debug_shell_help_command,
    },
    {
        .name = "wifi",
        .usage = "wifi status|set|clear",
        .summary = "Manage Wi-Fi credentials and show state",
        .details =
            "Status is read-only. Set prompts for an SSID and hidden password; "
            "clear removes stored Wi-Fi credentials.",
        .handler = airdap_debug_shell_wifi_command,
    },
    {
        .name = "button",
        .usage = "button simulate <single|double|hold2|hold6|hold10>|status|commands|bindings|defaults|bind <gesture> <command>",
        .summary = "Configure BOOT_KEY commands or simulate input",
        .details =
            "Simulate a complete BOOT_KEY gesture with automatic release, or show "
            "the queued/running gesture with status. commands lists the allowlist; bindings lists saved "
            "single/double/hold2/hold6/hold10 commands and the volatile DAP route. "
            "bind <gesture> <command> saves immediately; defaults saves the "
            "default bindings. Simulation takes real time, rejects overlapping requests, "
            "and is cancelled by physical input. Both use the same state machine.",
        .handler = airdap_debug_shell_button_command,
    },
    {
        .name = "swd-idcode",
        .usage = "swd-idcode [clock_khz]",
        .summary = "Read the target DP IDCODE",
        .details =
            "Acquires the diagnostic DAP owner, performs a bounded IDCODE "
            "probe at 100-10000 kHz, then releases the target-facing bus.",
        .handler = airdap_debug_shell_swd_idcode_command,
    },
    {
        .name = "restart",
        .usage = "restart",
        .summary = "Restart the AirDAP firmware",
        .details =
            "Waits for its acknowledgement to reach USB before restarting. "
            "The command fails without restarting if delivery times out.",
        .handler = airdap_debug_shell_restart_command,
    },
};

bool airdap_debug_shell_register_core_commands(
    airdap_debug_shell_command_registry_t *registry)
{
    for (size_t index = 0U;
         index < sizeof(core_commands) / sizeof(core_commands[0]);
         ++index) {
        if (airdap_debug_shell_command_register(
                registry,
                &core_commands[index]) !=
            AIRDAP_DEBUG_SHELL_COMMAND_REGISTERED) {
            return false;
        }
    }
    return true;
}
