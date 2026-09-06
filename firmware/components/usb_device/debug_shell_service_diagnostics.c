#include <inttypes.h>
#include <stddef.h>

#include "airdap_dap_service.h"
#include "airdap_debug_shell_commands.h"
#include "airdap_debug_shell_service_diagnostics.h"
#include "airdap_mode_state.h"
#include "airdap_wifi_manager.h"
#include "esp_err.h"

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

static const char *wifi_state_name(airdap_wifi_state_t state)
{
    switch (state) {
    case AIRDAP_WIFI_STOPPED:
        return "stopped";
    case AIRDAP_WIFI_DISCONNECTED:
        return "disconnected";
    case AIRDAP_WIFI_CONNECTING:
        return "connecting";
    case AIRDAP_WIFI_ONLINE:
        return "online";
    default:
        return "unknown";
    }
}

static const char *wifi_failure_name(airdap_wifi_manager_failure_t failure)
{
    switch (failure) {
    case AIRDAP_WIFI_MANAGER_FAILURE_NONE:
        return "none";
    case AIRDAP_WIFI_MANAGER_FAILURE_AUTHENTICATION:
        return "authentication";
    case AIRDAP_WIFI_MANAGER_FAILURE_TRANSIENT:
        return "transient";
    default:
        return "unknown";
    }
}

static int network_info_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context)
{
    (void) context;
    if (arguments == NULL || arguments[0] != '\0') {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_WARNING,
            "usage: network-info\n");
        return 1;
    }

    airdap_wifi_manager_info_t info;
    const esp_err_t info_error = airdap_wifi_manager_get_info(&info);
    if (info_error != ESP_OK) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "network-info: Wi-Fi status read failed: %s\n",
            esp_err_to_name(info_error));
        return 1;
    }
    airdap_mode_snapshot_t mode;
    const airdap_mode_state_result_t mode_result =
        airdap_mode_state_get(&mode);
    if (mode_result != AIRDAP_MODE_STATE_OK) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "network-info: mode status read failed: %u\n",
            (unsigned int) mode_result);
        return 1;
    }

    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "wifi=%s manager_started=%s configured=%s link_connected=%s "
        "provisioning_suspended=%s\n",
        wifi_state_name(mode.wifi),
        info.started ? "yes" : "no",
        info.has_configuration ? "yes" : "no",
        info.link_connected ? "yes" : "no",
        info.provisioning_suspended ? "yes" : "no");
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "last_failure=%s last_disconnect_reason=%u retry_delay_ms=%" PRIu32
        " retry_scheduled=%s\n",
        wifi_failure_name(info.last_failure),
        (unsigned int) info.last_disconnect_reason,
        info.retry_delay_ms,
        info.retry_scheduled ? "yes" : "no");
    if (info.ipv4_available) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
            "ipv4=%u.%u.%u.%u netmask=%u.%u.%u.%u gateway=%u.%u.%u.%u\n",
            info.ipv4_address[0], info.ipv4_address[1],
            info.ipv4_address[2], info.ipv4_address[3],
            info.ipv4_netmask[0], info.ipv4_netmask[1],
            info.ipv4_netmask[2], info.ipv4_netmask[3],
            info.ipv4_gateway[0], info.ipv4_gateway[1],
            info.ipv4_gateway[2], info.ipv4_gateway[3]);
    } else {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
            "ipv4=unavailable\n");
    }
    if (info.ap_available) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
            "ap rssi_dbm=%d channel=%u\n",
            (int) info.rssi_dbm,
            (unsigned int) info.channel);
    } else {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
            "ap=unavailable\n");
    }
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
    {
        .name = "network-info",
        .usage = "network-info",
        .summary = "Show non-secret Wi-Fi and IPv4 diagnostics",
        .details =
            "Reports Wi-Fi lifecycle, configuration presence, link state, "
            "failure class, retry state, IPv4 addressing, RSSI, and channel. "
            "SSID, BSSID, passwords, and authentication material are omitted.",
        .handler = network_info_command,
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
