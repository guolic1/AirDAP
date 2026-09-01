#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_wifi_credentials.h"
#include "airdap_wifi_disconnect_reason.h"
#include "airdap_wifi_manager.h"
#include "airdap_wifi_state_machine.h"
#include "esp_wifi.h"

static airdap_wifi_sm_effects_t step(
    airdap_wifi_state_machine_t *machine,
    airdap_wifi_sm_event_t event)
{
    airdap_wifi_sm_effects_t effects;
    assert(airdap_wifi_state_machine_step(machine, event, &effects));
    return effects;
}

static void test_no_configuration_stays_stopped(void)
{
    airdap_wifi_state_machine_t machine;
    airdap_wifi_state_machine_init(&machine, false);

    const airdap_wifi_sm_effects_t effects = step(
        &machine,
        AIRDAP_WIFI_SM_EVENT_STA_STARTED);

    assert(machine.state == AIRDAP_WIFI_SM_STOPPED);
    assert(!effects.connect);
    assert(!effects.publish_state);
    assert(effects.retry_after_ms == 0U);
}

static void test_link_connection_is_not_online_until_ip(void)
{
    airdap_wifi_state_machine_t machine;
    airdap_wifi_state_machine_init(&machine, true);

    airdap_wifi_sm_effects_t effects = step(
        &machine,
        AIRDAP_WIFI_SM_EVENT_STA_STARTED);
    assert(machine.state == AIRDAP_WIFI_SM_CONNECTING);
    assert(effects.connect);
    assert(effects.publish_state);

    effects = step(&machine, AIRDAP_WIFI_SM_EVENT_GOT_IP);
    assert(machine.state == AIRDAP_WIFI_SM_CONNECTING);
    assert(!effects.publish_state);

    effects = step(&machine, AIRDAP_WIFI_SM_EVENT_LINK_CONNECTED);
    assert(machine.state == AIRDAP_WIFI_SM_CONNECTING);
    assert(!effects.publish_state);

    effects = step(&machine, AIRDAP_WIFI_SM_EVENT_GOT_IP);
    assert(machine.state == AIRDAP_WIFI_SM_ONLINE);
    assert(effects.publish_state);
    assert(effects.published_state == AIRDAP_WIFI_SM_ONLINE);
    assert(machine.last_failure == AIRDAP_WIFI_FAILURE_NONE);
    assert(machine.retry_delay_ms == 0U);
}

static void test_lost_ip_revokes_online_until_dhcp_recovers(void)
{
    airdap_wifi_state_machine_t machine;
    airdap_wifi_state_machine_init(&machine, true);
    (void) step(&machine, AIRDAP_WIFI_SM_EVENT_STA_STARTED);
    (void) step(&machine, AIRDAP_WIFI_SM_EVENT_LINK_CONNECTED);
    (void) step(&machine, AIRDAP_WIFI_SM_EVENT_GOT_IP);

    airdap_wifi_sm_effects_t effects = step(
        &machine,
        AIRDAP_WIFI_SM_EVENT_LOST_IP);
    assert(machine.state == AIRDAP_WIFI_SM_CONNECTING);
    assert(machine.last_failure == AIRDAP_WIFI_FAILURE_TRANSIENT);
    assert(effects.publish_state);
    assert(effects.published_state == AIRDAP_WIFI_SM_CONNECTING);
    assert(effects.retry_after_ms == 0U);

    effects = step(&machine, AIRDAP_WIFI_SM_EVENT_GOT_IP);
    assert(machine.state == AIRDAP_WIFI_SM_ONLINE);
    assert(effects.published_state == AIRDAP_WIFI_SM_ONLINE);
}

static void test_wrong_password_is_distinct_and_backed_off(void)
{
    airdap_wifi_state_machine_t machine;
    airdap_wifi_state_machine_init(&machine, true);
    (void) step(&machine, AIRDAP_WIFI_SM_EVENT_STA_STARTED);

    const airdap_wifi_sm_effects_t effects = step(
        &machine,
        AIRDAP_WIFI_SM_EVENT_AUTHENTICATION_FAILED);

    assert(machine.state == AIRDAP_WIFI_SM_DISCONNECTED);
    assert(machine.last_failure == AIRDAP_WIFI_FAILURE_AUTHENTICATION);
    assert(effects.retry_after_ms == AIRDAP_WIFI_RETRY_INITIAL_DELAY_MS);
    assert(effects.publish_state);
}

static void test_connect_start_failure_is_transient_and_retried(void)
{
    airdap_wifi_state_machine_t machine;
    airdap_wifi_state_machine_init(&machine, true);
    (void) step(&machine, AIRDAP_WIFI_SM_EVENT_STA_STARTED);

    const airdap_wifi_sm_effects_t effects = step(
        &machine,
        AIRDAP_WIFI_SM_EVENT_CONNECT_FAILED);

    assert(machine.state == AIRDAP_WIFI_SM_DISCONNECTED);
    assert(machine.last_failure == AIRDAP_WIFI_FAILURE_TRANSIENT);
    assert(effects.retry_after_ms == AIRDAP_WIFI_RETRY_INITIAL_DELAY_MS);
}

static void test_esp_wifi_wrong_password_reasons_are_classified(void)
{
    assert(airdap_wifi_disconnect_is_authentication_failure(
        WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT));
    assert(airdap_wifi_disconnect_is_authentication_failure(
        WIFI_REASON_802_1X_AUTH_FAILED));
    assert(airdap_wifi_disconnect_is_authentication_failure(
        WIFI_REASON_AUTH_FAIL));
    assert(airdap_wifi_disconnect_is_authentication_failure(
        WIFI_REASON_HANDSHAKE_TIMEOUT));
    assert(!airdap_wifi_disconnect_is_authentication_failure(
        WIFI_REASON_BEACON_TIMEOUT));
    assert(!airdap_wifi_disconnect_is_authentication_failure(
        WIFI_REASON_NO_AP_FOUND));
}

static void test_transient_disconnect_uses_bounded_exponential_backoff(void)
{
    airdap_wifi_state_machine_t machine;
    airdap_wifi_state_machine_init(&machine, true);
    (void) step(&machine, AIRDAP_WIFI_SM_EVENT_STA_STARTED);

    uint32_t expected_delay = AIRDAP_WIFI_RETRY_INITIAL_DELAY_MS;
    for (size_t failure = 0U; failure < 10U; ++failure) {
        const airdap_wifi_sm_effects_t disconnected = step(
            &machine,
            AIRDAP_WIFI_SM_EVENT_TRANSIENT_DISCONNECT);
        assert(machine.last_failure == AIRDAP_WIFI_FAILURE_TRANSIENT);
        assert(disconnected.retry_after_ms == expected_delay);
        assert(disconnected.retry_after_ms <= AIRDAP_WIFI_RETRY_MAX_DELAY_MS);

        const airdap_wifi_sm_effects_t retry = step(
            &machine,
            AIRDAP_WIFI_SM_EVENT_RETRY_EXPIRED);
        assert(machine.state == AIRDAP_WIFI_SM_CONNECTING);
        assert(retry.connect);

        if (expected_delay < AIRDAP_WIFI_RETRY_MAX_DELAY_MS / 2U) {
            expected_delay *= 2U;
        } else {
            expected_delay = AIRDAP_WIFI_RETRY_MAX_DELAY_MS;
        }
    }
    assert(machine.retry_delay_ms == AIRDAP_WIFI_RETRY_MAX_DELAY_MS);
}

static void test_configuration_update_resets_backoff_and_reconnects_now(void)
{
    airdap_wifi_state_machine_t machine;
    airdap_wifi_state_machine_init(&machine, true);
    (void) step(&machine, AIRDAP_WIFI_SM_EVENT_STA_STARTED);
    (void) step(&machine, AIRDAP_WIFI_SM_EVENT_AUTHENTICATION_FAILED);
    (void) step(&machine, AIRDAP_WIFI_SM_EVENT_RETRY_EXPIRED);
    (void) step(&machine, AIRDAP_WIFI_SM_EVENT_AUTHENTICATION_FAILED);
    assert(machine.retry_delay_ms > AIRDAP_WIFI_RETRY_INITIAL_DELAY_MS);

    airdap_wifi_sm_effects_t effects = step(
        &machine,
        AIRDAP_WIFI_SM_EVENT_CONFIGURATION_UPDATED);

    assert(machine.state == AIRDAP_WIFI_SM_CONNECTING);
    assert(machine.last_failure == AIRDAP_WIFI_FAILURE_NONE);
    assert(machine.retry_delay_ms == 0U);
    assert(effects.cancel_retry);
    assert(effects.reconfigure);
    assert(effects.publish_state);

    effects = step(&machine, AIRDAP_WIFI_SM_EVENT_GOT_IP);
    assert(machine.state == AIRDAP_WIFI_SM_CONNECTING);
    assert(!effects.publish_state);
}

static void test_ap_recovery_returns_online_and_resets_backoff(void)
{
    airdap_wifi_state_machine_t machine;
    airdap_wifi_state_machine_init(&machine, true);
    (void) step(&machine, AIRDAP_WIFI_SM_EVENT_STA_STARTED);
    (void) step(&machine, AIRDAP_WIFI_SM_EVENT_TRANSIENT_DISCONNECT);

    airdap_wifi_sm_effects_t effects = step(
        &machine,
        AIRDAP_WIFI_SM_EVENT_RETRY_EXPIRED);
    assert(effects.connect);
    (void) step(&machine, AIRDAP_WIFI_SM_EVENT_LINK_CONNECTED);
    effects = step(&machine, AIRDAP_WIFI_SM_EVENT_GOT_IP);

    assert(machine.state == AIRDAP_WIFI_SM_ONLINE);
    assert(machine.retry_delay_ms == 0U);
    assert(machine.last_failure == AIRDAP_WIFI_FAILURE_NONE);
    assert(effects.published_state == AIRDAP_WIFI_SM_ONLINE);
}

static void test_configuration_clear_stops_retries(void)
{
    airdap_wifi_state_machine_t machine;
    airdap_wifi_state_machine_init(&machine, true);
    (void) step(&machine, AIRDAP_WIFI_SM_EVENT_STA_STARTED);
    (void) step(&machine, AIRDAP_WIFI_SM_EVENT_TRANSIENT_DISCONNECT);

    const airdap_wifi_sm_effects_t effects = step(
        &machine,
        AIRDAP_WIFI_SM_EVENT_CONFIGURATION_CLEARED);

    assert(machine.state == AIRDAP_WIFI_SM_STOPPED);
    assert(!machine.has_configuration);
    assert(machine.retry_delay_ms == 0U);
    assert(effects.cancel_retry);
    assert(effects.disconnect);
    assert(effects.publish_state);
}

static void test_credentials_round_trip_without_padding_or_terminators(void)
{
    const airdap_wifi_credentials_t input = {
        .ssid = {'A', 'i', 'r', 'D', 'A', 'P'},
        .ssid_length = 6U,
        .password = {'s', 'e', 'c', 'r', 'e', 't', '1', '2'},
        .password_length = 8U,
    };
    uint8_t encoded[AIRDAP_WIFI_CREDENTIALS_ENCODED_MAX_SIZE];
    size_t encoded_size = sizeof(encoded);

    assert(airdap_wifi_credentials_encode(&input, encoded, &encoded_size));
    assert(encoded_size == AIRDAP_WIFI_CREDENTIALS_HEADER_SIZE + 14U);

    airdap_wifi_credentials_t decoded;
    assert(airdap_wifi_credentials_decode(encoded, encoded_size, &decoded));
    assert(decoded.ssid_length == input.ssid_length);
    assert(decoded.password_length == input.password_length);
    assert(memcmp(decoded.ssid, input.ssid, input.ssid_length) == 0);
    assert(memcmp(decoded.password, input.password, input.password_length) == 0);

    encoded[0] ^= 0xFFU;
    assert(!airdap_wifi_credentials_decode(encoded, encoded_size, &decoded));
}

static void test_credentials_reject_invalid_lengths(void)
{
    airdap_wifi_credentials_t credentials = {
        .ssid_length = 0U,
        .password_length = 0U,
    };
    uint8_t encoded[AIRDAP_WIFI_CREDENTIALS_ENCODED_MAX_SIZE];
    size_t encoded_size = sizeof(encoded);

    assert(!airdap_wifi_credentials_encode(
        &credentials,
        encoded,
        &encoded_size));

    credentials.ssid_length = AIRDAP_WIFI_SSID_MAX_LENGTH + 1U;
    assert(!airdap_wifi_credentials_encode(
        &credentials,
        encoded,
        &encoded_size));

    credentials.ssid_length = 1U;
    credentials.password_length = AIRDAP_WIFI_PASSWORD_MAX_LENGTH + 1U;
    assert(!airdap_wifi_credentials_encode(
        &credentials,
        encoded,
        &encoded_size));
}

int main(void)
{
    test_no_configuration_stays_stopped();
    test_link_connection_is_not_online_until_ip();
    test_lost_ip_revokes_online_until_dhcp_recovers();
    test_wrong_password_is_distinct_and_backed_off();
    test_connect_start_failure_is_transient_and_retried();
    test_esp_wifi_wrong_password_reasons_are_classified();
    test_transient_disconnect_uses_bounded_exponential_backoff();
    test_configuration_update_resets_backoff_and_reconnects_now();
    test_ap_recovery_returns_online_and_resets_backoff();
    test_configuration_clear_stops_retries();
    test_credentials_round_trip_without_padding_or_terminators();
    test_credentials_reject_invalid_lengths();
    puts("wifi manager state-machine tests passed");
    return 0;
}
