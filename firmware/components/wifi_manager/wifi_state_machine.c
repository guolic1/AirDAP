#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "airdap_wifi_state_machine.h"

static void publish(
    airdap_wifi_sm_effects_t *effects,
    airdap_wifi_sm_state_t state)
{
    effects->publish_state = true;
    effects->published_state = state;
}

static uint32_t next_retry_delay(uint32_t current_delay)
{
    if (current_delay == 0U) {
        return AIRDAP_WIFI_RETRY_INITIAL_DELAY_MS;
    }
    if (current_delay >= AIRDAP_WIFI_RETRY_MAX_DELAY_MS / 2U) {
        return AIRDAP_WIFI_RETRY_MAX_DELAY_MS;
    }
    return current_delay * 2U;
}

static void handle_failure(
    airdap_wifi_state_machine_t *machine,
    airdap_wifi_failure_t failure,
    airdap_wifi_sm_effects_t *effects)
{
    if (!machine->has_configuration) {
        return;
    }
    machine->state = AIRDAP_WIFI_SM_DISCONNECTED;
    machine->last_failure = failure;
    machine->retry_delay_ms = next_retry_delay(machine->retry_delay_ms);
    effects->retry_after_ms = machine->retry_delay_ms;
    publish(effects, AIRDAP_WIFI_SM_DISCONNECTED);
}

void airdap_wifi_state_machine_init(
    airdap_wifi_state_machine_t *machine,
    bool has_configuration)
{
    if (machine == NULL) {
        return;
    }
    memset(machine, 0, sizeof(*machine));
    machine->state = AIRDAP_WIFI_SM_STOPPED;
    machine->has_configuration = has_configuration;
}

bool airdap_wifi_state_machine_step(
    airdap_wifi_state_machine_t *machine,
    airdap_wifi_sm_event_t event,
    airdap_wifi_sm_effects_t *effects)
{
    if (machine == NULL || effects == NULL) {
        return false;
    }
    memset(effects, 0, sizeof(*effects));

    switch (event) {
    case AIRDAP_WIFI_SM_EVENT_STA_STARTED:
        if (machine->has_configuration &&
            machine->state == AIRDAP_WIFI_SM_STOPPED) {
            machine->state = AIRDAP_WIFI_SM_CONNECTING;
            effects->connect = true;
            publish(effects, AIRDAP_WIFI_SM_CONNECTING);
        }
        break;
    case AIRDAP_WIFI_SM_EVENT_LINK_CONNECTED:
        break;
    case AIRDAP_WIFI_SM_EVENT_GOT_IP:
        if (machine->state == AIRDAP_WIFI_SM_CONNECTING) {
            machine->state = AIRDAP_WIFI_SM_ONLINE;
            machine->last_failure = AIRDAP_WIFI_FAILURE_NONE;
            machine->retry_delay_ms = 0U;
            publish(effects, AIRDAP_WIFI_SM_ONLINE);
        }
        break;
    case AIRDAP_WIFI_SM_EVENT_AUTHENTICATION_FAILED:
        handle_failure(
            machine,
            AIRDAP_WIFI_FAILURE_AUTHENTICATION,
            effects);
        break;
    case AIRDAP_WIFI_SM_EVENT_TRANSIENT_DISCONNECT:
    case AIRDAP_WIFI_SM_EVENT_CONNECT_FAILED:
        handle_failure(machine, AIRDAP_WIFI_FAILURE_TRANSIENT, effects);
        break;
    case AIRDAP_WIFI_SM_EVENT_RETRY_EXPIRED:
        if (machine->has_configuration &&
            machine->state == AIRDAP_WIFI_SM_DISCONNECTED) {
            machine->state = AIRDAP_WIFI_SM_CONNECTING;
            effects->connect = true;
            publish(effects, AIRDAP_WIFI_SM_CONNECTING);
        }
        break;
    case AIRDAP_WIFI_SM_EVENT_CONFIGURATION_UPDATED:
        machine->has_configuration = true;
        machine->state = AIRDAP_WIFI_SM_CONNECTING;
        machine->last_failure = AIRDAP_WIFI_FAILURE_NONE;
        machine->retry_delay_ms = 0U;
        effects->cancel_retry = true;
        effects->reconfigure = true;
        publish(effects, AIRDAP_WIFI_SM_CONNECTING);
        break;
    case AIRDAP_WIFI_SM_EVENT_CONFIGURATION_CLEARED:
        machine->has_configuration = false;
        machine->state = AIRDAP_WIFI_SM_STOPPED;
        machine->last_failure = AIRDAP_WIFI_FAILURE_NONE;
        machine->retry_delay_ms = 0U;
        effects->cancel_retry = true;
        effects->disconnect = true;
        publish(effects, AIRDAP_WIFI_SM_STOPPED);
        break;
    default:
        return false;
    }
    return true;
}
