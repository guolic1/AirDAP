#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_debug_shell_wifi.h"
#include "airdap_mode_state.h"
#include "esp_err.h"

enum {
    WIFI_INPUT_IDLE = 0,
    WIFI_INPUT_SSID,
    WIFI_INPUT_PASSWORD,
};

static void clear_bytes(void *data, size_t size)
{
    volatile uint8_t *byte = (volatile uint8_t *) data;
    for (size_t index = 0U; index < size; ++index) {
        byte[index] = 0U;
    }
}

static bool format_output(
    char *output,
    size_t output_size,
    const char *format,
    ...)
{
    if (output == NULL || output_size == 0U || format == NULL) {
        return false;
    }

    va_list arguments;
    va_start(arguments, format);
    const int formatted = vsnprintf(output, output_size, format, arguments);
    va_end(arguments);
    return formatted >= 0 && (size_t) formatted < output_size;
}

static bool is_printable_ascii(const char *value, size_t length)
{
    for (size_t index = 0U; index < length; ++index) {
        const uint8_t byte = (uint8_t) value[index];
        if (byte < 0x20U || byte > 0x7EU) {
            return false;
        }
    }
    return true;
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
        return NULL;
    }
}

void airdap_debug_shell_wifi_session_init(
    airdap_debug_shell_wifi_session_t *session)
{
    if (session != NULL) {
        clear_bytes(session, sizeof(*session));
    }
}

void airdap_debug_shell_wifi_cancel(
    airdap_debug_shell_wifi_session_t *session)
{
    airdap_debug_shell_wifi_session_init(session);
}

bool airdap_debug_shell_wifi_input_pending(
    const airdap_debug_shell_wifi_session_t *session)
{
    return session != NULL && session->input_stage != WIFI_INPUT_IDLE;
}

bool airdap_debug_shell_wifi_input_is_secret(
    const airdap_debug_shell_wifi_session_t *session)
{
    return session != NULL && session->input_stage == WIFI_INPUT_PASSWORD;
}

const char *airdap_debug_shell_wifi_input_prompt(
    const airdap_debug_shell_wifi_session_t *session)
{
    if (session == NULL) {
        return NULL;
    }
    if (session->input_stage == WIFI_INPUT_SSID) {
        return "SSID: ";
    }
    if (session->input_stage == WIFI_INPUT_PASSWORD) {
        return "Password: ";
    }
    return NULL;
}

static int execute_status(
    char *output,
    size_t output_size,
    airdap_debug_shell_wifi_style_t *style)
{
    airdap_mode_snapshot_t snapshot;
    const airdap_mode_state_result_t result = airdap_mode_state_get(&snapshot);
    if (result != AIRDAP_MODE_STATE_OK) {
        *style = AIRDAP_DEBUG_SHELL_WIFI_STYLE_RED;
        (void) format_output(
            output,
            output_size,
            "wifi: state read failed: %d\n",
            result);
        return 1;
    }

    const char *state_name = wifi_state_name(snapshot.wifi);
    if (state_name == NULL) {
        *style = AIRDAP_DEBUG_SHELL_WIFI_STYLE_RED;
        (void) format_output(output, output_size, "wifi: invalid state\n");
        return 1;
    }
    *style = snapshot.wifi == AIRDAP_WIFI_ONLINE
        ? AIRDAP_DEBUG_SHELL_WIFI_STYLE_GREEN
        : AIRDAP_DEBUG_SHELL_WIFI_STYLE_YELLOW;
    if (!format_output(output, output_size, "wifi=%s\n", state_name)) {
        *style = AIRDAP_DEBUG_SHELL_WIFI_STYLE_RED;
        return 1;
    }
    return 0;
}

static int execute_clear(
    char *output,
    size_t output_size,
    airdap_debug_shell_wifi_style_t *style)
{
    const esp_err_t error = airdap_wifi_manager_clear_credentials();
    if (error != ESP_OK) {
        *style = AIRDAP_DEBUG_SHELL_WIFI_STYLE_RED;
        (void) format_output(
            output,
            output_size,
            "wifi: clear failed: %s\n",
            esp_err_to_name(error));
        return 1;
    }
    *style = AIRDAP_DEBUG_SHELL_WIFI_STYLE_GREEN;
    return format_output(
        output,
        output_size,
        "wifi: credentials cleared\n") ? 0 : 1;
}

int airdap_debug_shell_wifi_execute(
    airdap_debug_shell_wifi_session_t *session,
    const char *arguments,
    char *output,
    size_t output_size,
    airdap_debug_shell_wifi_style_t *style)
{
    if (session == NULL || arguments == NULL || output == NULL ||
        output_size == 0U || style == NULL) {
        return 1;
    }
    if (strcmp(arguments, "status") == 0) {
        return execute_status(output, output_size, style);
    }
    if (strcmp(arguments, "clear") == 0) {
        airdap_debug_shell_wifi_cancel(session);
        return execute_clear(output, output_size, style);
    }
    if (strcmp(arguments, "set") == 0) {
        airdap_debug_shell_wifi_cancel(session);
        session->input_stage = WIFI_INPUT_SSID;
        *style = AIRDAP_DEBUG_SHELL_WIFI_STYLE_YELLOW;
        return format_output(
            output,
            output_size,
            "wifi: enter SSID; Ctrl-C cancels\n") ? 0 : 1;
    }

    *style = AIRDAP_DEBUG_SHELL_WIFI_STYLE_YELLOW;
    (void) format_output(
        output,
        output_size,
        "usage: wifi status|set|clear\n");
    return 1;
}

static int submit_ssid(
    airdap_debug_shell_wifi_session_t *session,
    const char *line,
    char *output,
    size_t output_size,
    airdap_debug_shell_wifi_style_t *style)
{
    const size_t length = strlen(line);
    if (length == 0U || length > AIRDAP_WIFI_SSID_MAX_LENGTH ||
        !is_printable_ascii(line, length)) {
        *style = AIRDAP_DEBUG_SHELL_WIFI_STYLE_YELLOW;
        (void) format_output(
            output,
            output_size,
            "wifi: SSID must be 1..32 printable ASCII bytes\n");
        return 1;
    }

    memcpy(session->ssid, line, length);
    session->ssid_length = (uint8_t) length;
    session->input_stage = WIFI_INPUT_PASSWORD;
    *style = AIRDAP_DEBUG_SHELL_WIFI_STYLE_YELLOW;
    return format_output(
        output,
        output_size,
        "wifi: enter password; Ctrl-C cancels\n") ? 0 : 1;
}

static int submit_password(
    airdap_debug_shell_wifi_session_t *session,
    const char *line,
    char *output,
    size_t output_size,
    airdap_debug_shell_wifi_style_t *style)
{
    const size_t length = strlen(line);
    if (length > AIRDAP_WIFI_PASSWORD_MAX_LENGTH ||
        !is_printable_ascii(line, length)) {
        *style = AIRDAP_DEBUG_SHELL_WIFI_STYLE_YELLOW;
        (void) format_output(
            output,
            output_size,
            "wifi: password must be 0..64 printable ASCII bytes\n");
        return 1;
    }

    airdap_wifi_credentials_t credentials = {0};
    memcpy(credentials.ssid, session->ssid, session->ssid_length);
    credentials.ssid_length = session->ssid_length;
    memcpy(credentials.password, line, length);
    credentials.password_length = (uint8_t) length;
    const esp_err_t error = airdap_wifi_manager_set_credentials(&credentials);
    clear_bytes(&credentials, sizeof(credentials));
    airdap_debug_shell_wifi_cancel(session);

    if (error != ESP_OK) {
        *style = AIRDAP_DEBUG_SHELL_WIFI_STYLE_RED;
        (void) format_output(
            output,
            output_size,
            "wifi: save failed: %s\n",
            esp_err_to_name(error));
        return 1;
    }
    *style = AIRDAP_DEBUG_SHELL_WIFI_STYLE_GREEN;
    return format_output(
        output,
        output_size,
        "wifi: credentials saved; reconnect requested\n") ? 0 : 1;
}

int airdap_debug_shell_wifi_submit(
    airdap_debug_shell_wifi_session_t *session,
    const char *line,
    char *output,
    size_t output_size,
    airdap_debug_shell_wifi_style_t *style)
{
    if (session == NULL || line == NULL || output == NULL ||
        output_size == 0U || style == NULL) {
        return 1;
    }
    if (session->input_stage == WIFI_INPUT_SSID) {
        return submit_ssid(session, line, output, output_size, style);
    }
    if (session->input_stage == WIFI_INPUT_PASSWORD) {
        return submit_password(session, line, output, output_size, style);
    }

    *style = AIRDAP_DEBUG_SHELL_WIFI_STYLE_RED;
    (void) format_output(
        output,
        output_size,
        "wifi: no credential input pending\n");
    return 1;
}
