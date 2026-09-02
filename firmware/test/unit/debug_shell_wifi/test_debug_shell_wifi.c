#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_debug_shell_wifi.h"
#include "airdap_mode_state.h"

static airdap_mode_state_result_t mode_result;
static airdap_wifi_state_t wifi_state;
static esp_err_t set_result;
static esp_err_t clear_result;
static airdap_wifi_credentials_t captured_credentials;
static size_t set_calls;
static size_t clear_calls;

airdap_mode_state_result_t airdap_mode_state_get(
    airdap_mode_snapshot_t *snapshot)
{
    if (mode_result == AIRDAP_MODE_STATE_OK) {
        snapshot->wifi = wifi_state;
    }
    return mode_result;
}

esp_err_t airdap_wifi_manager_set_credentials(
    const airdap_wifi_credentials_t *credentials)
{
    ++set_calls;
    captured_credentials = *credentials;
    return set_result;
}

esp_err_t airdap_wifi_manager_clear_credentials(void)
{
    ++clear_calls;
    return clear_result;
}

const char *esp_err_to_name(esp_err_t error)
{
    return error == ESP_FAIL ? "ESP_FAIL" : "ESP_UNKNOWN";
}

static void reset_fakes(void)
{
    mode_result = AIRDAP_MODE_STATE_OK;
    wifi_state = AIRDAP_WIFI_STOPPED;
    set_result = ESP_OK;
    clear_result = ESP_OK;
    memset(&captured_credentials, 0, sizeof(captured_credentials));
    set_calls = 0U;
    clear_calls = 0U;
}

static int execute(
    airdap_debug_shell_wifi_session_t *session,
    const char *arguments,
    char *output,
    airdap_debug_shell_wifi_style_t *style)
{
    return airdap_debug_shell_wifi_execute(
        session,
        arguments,
        output,
        AIRDAP_DEBUG_SHELL_WIFI_OUTPUT_SIZE,
        style);
}

static int submit(
    airdap_debug_shell_wifi_session_t *session,
    const char *line,
    char *output,
    airdap_debug_shell_wifi_style_t *style)
{
    return airdap_debug_shell_wifi_submit(
        session,
        line,
        output,
        AIRDAP_DEBUG_SHELL_WIFI_OUTPUT_SIZE,
        style);
}

static void assert_session_scrubbed(
    const airdap_debug_shell_wifi_session_t *session)
{
    assert(!airdap_debug_shell_wifi_input_pending(session));
    assert(session->ssid_length == 0U);
    for (size_t index = 0U; index < sizeof(session->ssid); ++index) {
        assert(session->ssid[index] == 0U);
    }
}

static void test_status_reports_each_runtime_state(void)
{
    static const char *const expected[] = {
        "wifi=stopped\n",
        "wifi=disconnected\n",
        "wifi=connecting\n",
        "wifi=online\n",
    };
    airdap_debug_shell_wifi_session_t session;
    char output[AIRDAP_DEBUG_SHELL_WIFI_OUTPUT_SIZE];
    airdap_debug_shell_wifi_style_t style;

    reset_fakes();
    airdap_debug_shell_wifi_session_init(&session);
    for (size_t index = 0U; index < sizeof(expected) / sizeof(expected[0]); ++index) {
        wifi_state = (airdap_wifi_state_t) index;
        assert(execute(&session, "status", output, &style) == 0);
        assert(strcmp(output, expected[index]) == 0);
        assert(style == (wifi_state == AIRDAP_WIFI_ONLINE
            ? AIRDAP_DEBUG_SHELL_WIFI_STYLE_GREEN
            : AIRDAP_DEBUG_SHELL_WIFI_STYLE_YELLOW));
    }

    mode_result = AIRDAP_MODE_STATE_INVALID_STATE;
    assert(execute(&session, "status", output, &style) == 1);
    assert(strcmp(output, "wifi: state read failed: 2\n") == 0);
    assert(style == AIRDAP_DEBUG_SHELL_WIFI_STYLE_RED);
}

static void test_set_collects_credentials_without_outputting_them(void)
{
    airdap_debug_shell_wifi_session_t session;
    char output[AIRDAP_DEBUG_SHELL_WIFI_OUTPUT_SIZE];
    airdap_debug_shell_wifi_style_t style;

    reset_fakes();
    airdap_debug_shell_wifi_session_init(&session);
    assert(execute(&session, "set", output, &style) == 0);
    assert(strcmp(output, "wifi: enter SSID; Ctrl-C cancels\n") == 0);
    assert(strcmp(airdap_debug_shell_wifi_input_prompt(&session), "SSID: ") == 0);
    assert(!airdap_debug_shell_wifi_input_is_secret(&session));

    assert(submit(&session, "Lab AP", output, &style) == 0);
    assert(strcmp(output, "wifi: enter password; Ctrl-C cancels\n") == 0);
    assert(strcmp(airdap_debug_shell_wifi_input_prompt(&session), "Password: ") == 0);
    assert(airdap_debug_shell_wifi_input_is_secret(&session));

    assert(submit(&session, "test password", output, &style) == 0);
    assert(set_calls == 1U);
    assert(captured_credentials.ssid_length == 6U);
    assert(memcmp(captured_credentials.ssid, "Lab AP", 6U) == 0);
    assert(captured_credentials.password_length == 13U);
    assert(memcmp(captured_credentials.password, "test password", 13U) == 0);
    assert(strcmp(output, "wifi: credentials saved; reconnect requested\n") == 0);
    assert(strstr(output, "Lab AP") == NULL);
    assert(strstr(output, "test password") == NULL);
    assert(style == AIRDAP_DEBUG_SHELL_WIFI_STYLE_GREEN);
    assert_session_scrubbed(&session);
}

static void test_empty_password_is_supported(void)
{
    airdap_debug_shell_wifi_session_t session;
    char output[AIRDAP_DEBUG_SHELL_WIFI_OUTPUT_SIZE];
    airdap_debug_shell_wifi_style_t style;

    reset_fakes();
    airdap_debug_shell_wifi_session_init(&session);
    assert(execute(&session, "set", output, &style) == 0);
    assert(submit(&session, "open-ap", output, &style) == 0);
    assert(submit(&session, "", output, &style) == 0);
    assert(set_calls == 1U);
    assert(captured_credentials.password_length == 0U);
    assert_session_scrubbed(&session);
}

static void test_invalid_input_reprompts_without_committing(void)
{
    airdap_debug_shell_wifi_session_t session;
    char output[AIRDAP_DEBUG_SHELL_WIFI_OUTPUT_SIZE];
    airdap_debug_shell_wifi_style_t style;
    char long_ssid[AIRDAP_WIFI_SSID_MAX_LENGTH + 2U];
    char long_password[AIRDAP_WIFI_PASSWORD_MAX_LENGTH + 2U];

    reset_fakes();
    memset(long_ssid, 's', sizeof(long_ssid) - 1U);
    long_ssid[sizeof(long_ssid) - 1U] = '\0';
    memset(long_password, 'p', sizeof(long_password) - 1U);
    long_password[sizeof(long_password) - 1U] = '\0';

    airdap_debug_shell_wifi_session_init(&session);
    assert(execute(&session, "set", output, &style) == 0);
    assert(submit(&session, "", output, &style) == 1);
    assert(strcmp(output, "wifi: SSID must be 1..32 printable ASCII bytes\n") == 0);
    assert(strcmp(airdap_debug_shell_wifi_input_prompt(&session), "SSID: ") == 0);
    assert(submit(&session, long_ssid, output, &style) == 1);
    assert(set_calls == 0U);

    assert(submit(&session, "valid", output, &style) == 0);
    assert(submit(&session, long_password, output, &style) == 1);
    assert(strcmp(
        output,
        "wifi: password must be 0..64 printable ASCII bytes\n") == 0);
    assert(airdap_debug_shell_wifi_input_is_secret(&session));
    assert(set_calls == 0U);
    airdap_debug_shell_wifi_cancel(&session);
    assert_session_scrubbed(&session);
}

static void test_set_failure_and_cancel_scrub_session(void)
{
    airdap_debug_shell_wifi_session_t session;
    char output[AIRDAP_DEBUG_SHELL_WIFI_OUTPUT_SIZE];
    airdap_debug_shell_wifi_style_t style;

    reset_fakes();
    set_result = ESP_FAIL;
    airdap_debug_shell_wifi_session_init(&session);
    assert(execute(&session, "set", output, &style) == 0);
    assert(submit(&session, "private-ap", output, &style) == 0);
    assert(submit(&session, "private-password", output, &style) == 1);
    assert(strcmp(output, "wifi: save failed: ESP_FAIL\n") == 0);
    assert(strstr(output, "private") == NULL);
    assert(style == AIRDAP_DEBUG_SHELL_WIFI_STYLE_RED);
    assert_session_scrubbed(&session);

    assert(execute(&session, "set", output, &style) == 0);
    assert(submit(&session, "cancelled-ap", output, &style) == 0);
    airdap_debug_shell_wifi_cancel(&session);
    assert_session_scrubbed(&session);
}

static void test_clear_and_usage_are_observable(void)
{
    airdap_debug_shell_wifi_session_t session;
    char output[AIRDAP_DEBUG_SHELL_WIFI_OUTPUT_SIZE];
    airdap_debug_shell_wifi_style_t style;

    reset_fakes();
    airdap_debug_shell_wifi_session_init(&session);
    assert(execute(&session, "clear", output, &style) == 0);
    assert(clear_calls == 1U);
    assert(strcmp(output, "wifi: credentials cleared\n") == 0);
    assert(style == AIRDAP_DEBUG_SHELL_WIFI_STYLE_GREEN);

    clear_result = ESP_FAIL;
    assert(execute(&session, "clear", output, &style) == 1);
    assert(clear_calls == 2U);
    assert(strcmp(output, "wifi: clear failed: ESP_FAIL\n") == 0);
    assert(style == AIRDAP_DEBUG_SHELL_WIFI_STYLE_RED);

    assert(execute(&session, "", output, &style) == 1);
    assert(strcmp(output, "usage: wifi status|set|clear\n") == 0);
    assert(execute(&session, "set extra", output, &style) == 1);
    assert(set_calls == 0U);
}

int main(void)
{
    test_status_reports_each_runtime_state();
    test_set_collects_credentials_without_outputting_them();
    test_empty_password_is_supported();
    test_invalid_input_reprompts_without_committing();
    test_set_failure_and_cancel_scrub_session();
    test_clear_and_usage_are_observable();

    puts("Debug shell Wi-Fi tests passed");
    return 0;
}
