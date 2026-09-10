#include <inttypes.h>
#include <stddef.h>

#include "airdap_dap_service.h"
#include "airdap_debug_shell_commands.h"
#include "airdap_debug_shell_service_diagnostics.h"
#include "airdap_discovery.h"
#include "airdap_mode_state.h"
#include "airdap_network_auth.h"
#include "airdap_network_dap.h"
#include "airdap_target_uart.h"
#include "airdap_usb_status.h"
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

static const char *dap_owner_name(airdap_dap_owner_t owner)
{
    switch (owner) {
    case AIRDAP_DAP_OWNER_NONE:
        return "none";
    case AIRDAP_DAP_OWNER_USB:
        return "usb";
    case AIRDAP_DAP_OWNER_NETWORK:
        return "network";
    case AIRDAP_DAP_OWNER_DIAGNOSTIC:
        return "diagnostic";
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

static int network_status_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context)
{
    (void) context;
    if (arguments == NULL || arguments[0] != '\0') {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_WARNING,
            "usage: network-status\n");
        return 1;
    }

    airdap_wifi_manager_info_t wifi_info;
    const esp_err_t wifi_error = airdap_wifi_manager_get_info(&wifi_info);
    if (wifi_error != ESP_OK) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "network-status: Wi-Fi status read failed: %s\n",
            esp_err_to_name(wifi_error));
        return 1;
    }
    airdap_mode_snapshot_t mode;
    const airdap_mode_state_result_t mode_result =
        airdap_mode_state_get(&mode);
    if (mode_result != AIRDAP_MODE_STATE_OK) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "network-status: mode status read failed: %u\n",
            (unsigned int) mode_result);
        return 1;
    }
    airdap_discovery_status_t discovery;
    const esp_err_t discovery_error = airdap_discovery_get_status(&discovery);
    if (discovery_error != ESP_OK) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "network-status: discovery status read failed: %s\n",
            esp_err_to_name(discovery_error));
        return 1;
    }
    airdap_network_dap_status_t dap;
    const esp_err_t dap_error = airdap_network_dap_get_status(&dap);
    if (dap_error != ESP_OK) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "network-status: DAP listener status read failed: %s\n",
            esp_err_to_name(dap_error));
        return 1;
    }

    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "wifi_online=%s ipv4=",
        mode.wifi == AIRDAP_WIFI_ONLINE ? "yes" : "no");
    if (wifi_info.ipv4_available) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
            "%u.%u.%u.%u",
            wifi_info.ipv4_address[0],
            wifi_info.ipv4_address[1],
            wifi_info.ipv4_address[2],
            wifi_info.ipv4_address[3]);
    } else {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
            "unavailable");
    }
    if (wifi_info.ap_available) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
            " rssi_dbm=%d\n",
            (int) wifi_info.rssi_dbm);
    } else {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
            " rssi_dbm=unavailable\n");
    }
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "mdns_published=%s dap_listener_ready=%s dap_owner=%s\n",
        discovery.service_published ? "yes" : "no",
        dap.listener_ready ? "yes" : "no",
        dap_owner_name(mode.dap_owner));
    return 0;
}

static int sessions_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context)
{
    (void) context;
    if (arguments == NULL || arguments[0] != '\0') {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_WARNING,
            "usage: sessions\n");
        return 1;
    }

    airdap_network_auth_status_t auth;
    const esp_err_t auth_error = airdap_network_auth_get_status(&auth);
    if (auth_error != ESP_OK) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "sessions: authentication status read failed: %s\n",
            esp_err_to_name(auth_error));
        return 1;
    }
    airdap_network_dap_status_t dap;
    const esp_err_t dap_error = airdap_network_dap_get_status(&dap);
    if (dap_error != ESP_OK) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "sessions: DAP listener status read failed: %s\n",
            esp_err_to_name(dap_error));
        return 1;
    }

    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "auth credential_present=%s pending_handshakes=%u "
        "logical_owner_active=%s bound_connections=%u\n",
        auth.credential_present ? "yes" : "no",
        auth.pending_handshakes,
        auth.logical_owner_active ? "yes" : "no",
        auth.bound_connections);
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "dap listener_ready=%s allocated_connections=%u tls_connections=%u "
        "authenticated_connections=%u dap_sessions=%u\n",
        dap.listener_ready ? "yes" : "no",
        dap.allocated_connections,
        dap.tls_connections,
        dap.authenticated_connections,
        dap.dap_sessions);
    return 0;
}

static int usb_status_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context)
{
    (void) context;
    if (arguments == NULL || arguments[0] != '\0') {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_WARNING,
            "usage: usb-status\n");
        return 1;
    }

    airdap_usb_status_t status;
    airdap_usb_get_status(&status);
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "bus_mounted=%s suspended=%s dap_vendor_mounted=%s "
        "target_cdc_connected=%s debug_vendor_mounted=%s "
        "dap_session_active=%s\n",
        status.bus_mounted ? "yes" : "no",
        status.suspended ? "yes" : "no",
        status.dap_vendor_mounted ? "yes" : "no",
        status.target_cdc_connected ? "yes" : "no",
        status.debug_vendor_mounted ? "yes" : "no",
        status.dap_session_active ? "yes" : "no");
    return 0;
}

static const char *uart_parity_name(uint8_t parity)
{
    switch (parity) {
    case 0U:
        return "none";
    case 1U:
        return "odd";
    case 2U:
        return "even";
    default:
        return "unknown";
    }
}

static const char *uart_stop_bits_name(uint8_t stop_bits)
{
    switch (stop_bits) {
    case 0U:
        return "1";
    case 1U:
        return "1.5";
    case 2U:
        return "2";
    default:
        return "unknown";
    }
}

static int uart_status_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context)
{
    (void) context;
    if (arguments == NULL || arguments[0] != '\0') {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_WARNING,
            "usage: uart-status\n");
        return 1;
    }

    airdap_target_uart_status_t status;
    const esp_err_t error = airdap_target_uart_get_status(&status);
    if (error != ESP_OK) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "uart-status: status read failed: %s\n",
            esp_err_to_name(error));
        return 1;
    }
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "initialized=%s baud=%" PRIu32 " data_bits=%u parity=%s "
        "stop_bits=%s\n",
        status.initialized ? "yes" : "no",
        status.baud_rate,
        (unsigned int) status.data_bits,
        uart_parity_name(status.parity),
        uart_stop_bits_name(status.stop_bits));
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "buffers rx_queued=%zu tx_free=%zu\n",
        status.rx_buffered_bytes,
        status.tx_buffer_free_bytes);
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "counters rx_bytes=%" PRIu32 " tx_bytes=%" PRIu32
        " read_failures=%" PRIu32 " write_failures=%" PRIu32 "\n",
        status.rx_bytes,
        status.tx_bytes,
        status.read_failures,
        status.write_failures);
    return 0;
}

static int discovery_status_command(
    const char *arguments,
    const airdap_debug_shell_invocation_t *invocation,
    void *context)
{
    (void) context;
    if (arguments == NULL || arguments[0] != '\0') {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_WARNING,
            "usage: discovery-status\n");
        return 1;
    }

    airdap_discovery_status_t status;
    const esp_err_t error = airdap_discovery_get_status(&status);
    if (error != ESP_OK) {
        airdap_debug_shell_printf(
            invocation,
            AIRDAP_DEBUG_SHELL_STYLE_ERROR,
            "discovery-status: status read failed: %s\n",
            esp_err_to_name(error));
        return 1;
    }
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "mdns_initialized=%s started=%s service_published=%s hostname=%s\n",
        status.initialized ? "yes" : "no",
        status.started ? "yes" : "no",
        status.service_published ? "yes" : "no",
        status.hostname[0] != '\0' ? status.hostname : "unavailable");
    airdap_debug_shell_printf(
        invocation,
        AIRDAP_DEBUG_SHELL_STYLE_SUCCESS,
        "service=_airdap._tcp dap_port=%u uart_port=%u last_error=%s\n",
        (unsigned int) status.dap_port,
        (unsigned int) status.uart_port,
        esp_err_to_name(status.last_error));
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
    {
        .name = "network-status",
        .usage = "network-status",
        .summary = "Show aggregate network service readiness",
        .details =
            "Reports Wi-Fi online state, IPv4 address, RSSI, mDNS "
            "publication, authenticated DAP listener readiness, and the "
            "current DAP owner without changing network state.",
        .handler = network_status_command,
    },
    {
        .name = "sessions",
        .usage = "sessions",
        .summary = "Show non-secret authentication and session counts",
        .details =
            "Reports credential presence, pending TLS handshakes, logical "
            "owner state, bound authentication connections, and allocated, "
            "TLS, authenticated, and DAP connection counts. Credentials, "
            "fingerprints, keys, and session tokens are omitted.",
        .handler = sessions_command,
    },
    {
        .name = "usb-status",
        .usage = "usb-status",
        .summary = "Show USB bus, interface, and DAP session state",
        .details =
            "Reports TinyUSB mount and suspend state, the DAP Vendor, target "
            "CDC, and debug Vendor interfaces, and whether a USB DAP service "
            "session is active without changing the USB connection.",
        .handler = usb_status_command,
    },
    {
        .name = "uart-status",
        .usage = "uart-status",
        .summary = "Show target UART configuration and I/O counters",
        .details =
            "Reports the last accepted line coding, RX queued and TX free "
            "bytes, transferred-byte totals, and driver read/write failures "
            "without reading or draining target UART data.",
        .handler = uart_status_command,
    },
    {
        .name = "discovery-status",
        .usage = "discovery-status",
        .summary = "Show mDNS discovery lifecycle and service state",
        .details =
            "Reports mDNS initialization, lifecycle and publication state, "
            "the advertised hostname and service ports, and the latest "
            "lifecycle error without publishing or withdrawing the service.",
        .handler = discovery_status_command,
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
