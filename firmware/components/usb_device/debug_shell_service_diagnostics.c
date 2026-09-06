#include <inttypes.h>
#include <stddef.h>

#include "airdap_dap_service.h"
#include "airdap_debug_shell_commands.h"
#include "airdap_debug_shell_service_diagnostics.h"

static int dap_stats_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context)
{
    (void) context;
    if (arguments == NULL || arguments[0] != '\0') {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_WARNING,
            "usage: dap-stats\n");
        return 1;
    }

    airdap_dap_service_stats_t stats;
    airdap_dap_service_get_stats(&stats);
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "requests accepted=%" PRIu32 " processed=%" PRIu32
        " responses_delivered=%" PRIu32 "\n",
        stats.requests_accepted,
        stats.requests_processed,
        stats.responses_delivered);
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "failures queue_full=%" PRIu32 " timed_out=%" PRIu32
        " stale_requests=%" PRIu32 " stale_responses=%" PRIu32
        " delivery=%" PRIu32 "\n",
        stats.queue_full,
        stats.timed_out,
        stats.stale_requests,
        stats.stale_responses,
        stats.delivery_failures);
    return 0;
}

static const airdap_debug_shell_command_t service_diagnostic_commands[] = {
    {
        .name = "dap-stats",
        .usage = "dap-stats",
        .summary = "Show DAP service request and failure counters",
        .details =
            "Reports accepted and processed requests, delivered responses, "
            "queue saturation, timeouts, stale work, and delivery failures "
            "without resetting the counters.",
        .handler = dap_stats_command,
    },
};

bool airdap_debug_shell_register_service_diagnostic_commands(
    airdap_debug_shell_command_registry_t *registry)
{
    for (size_t index = 0U;
         index < sizeof(service_diagnostic_commands) /
            sizeof(service_diagnostic_commands[0]);
         ++index) {
        if (airdap_debug_shell_command_register(
                registry,
                &service_diagnostic_commands[index]) !=
            AIRDAP_DEBUG_SHELL_COMMAND_REGISTERED) {
            return false;
        }
    }
    return true;
}
