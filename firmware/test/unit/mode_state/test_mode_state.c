#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "airdap_mode_state.h"

static airdap_dap_owner_t current_owner;
static unsigned acquire_calls;
static unsigned conditional_revoke_calls;
static unsigned release_calls;
static unsigned operation_begin_calls;
static unsigned operation_end_calls;
static bool attach_during_acquire;
static bool attach_during_operation_begin;
static bool wifi_disconnect_during_acquire;
static bool wifi_disconnect_during_operation_begin;
static bool operation_active;

airdap_dap_owner_t airdap_dap_ownership_current(void)
{
    return current_owner;
}

airdap_dap_ownership_result_t airdap_dap_ownership_acquire(
    airdap_dap_owner_t owner,
    airdap_dap_ownership_claim_t *claim)
{
    assert(claim != NULL);
    ++acquire_calls;
    if (attach_during_acquire) {
        attach_during_acquire = false;
        assert(airdap_mode_state_transition(
            AIRDAP_MODE_EVENT_USB_ATTACHED) == AIRDAP_MODE_STATE_OK);
    }
    if (wifi_disconnect_during_acquire) {
        wifi_disconnect_during_acquire = false;
        assert(airdap_mode_state_transition(
            AIRDAP_MODE_EVENT_WIFI_DISCONNECTED) == AIRDAP_MODE_STATE_OK);
    }
    if (current_owner == owner) {
        claim->owner = owner;
        claim->generation = 1U;
        return AIRDAP_DAP_OWNERSHIP_ALREADY_OWNER;
    }
    if (current_owner != AIRDAP_DAP_OWNER_NONE) {
        return AIRDAP_DAP_OWNERSHIP_BUSY;
    }
    current_owner = owner;
    claim->owner = owner;
    claim->generation = 1U;
    return AIRDAP_DAP_OWNERSHIP_OK;
}

airdap_dap_ownership_result_t airdap_dap_ownership_revoke_owner(
    airdap_dap_owner_t owner)
{
    ++conditional_revoke_calls;
    if (operation_active) {
        return AIRDAP_DAP_OWNERSHIP_BUSY;
    }
    if (current_owner != owner) {
        return AIRDAP_DAP_OWNERSHIP_NOT_OWNER;
    }
    current_owner = AIRDAP_DAP_OWNER_NONE;
    return AIRDAP_DAP_OWNERSHIP_OK;
}

airdap_dap_ownership_result_t airdap_dap_ownership_release(
    const airdap_dap_ownership_claim_t *claim)
{
    assert(claim != NULL);
    ++release_calls;
    if (operation_active) {
        return AIRDAP_DAP_OWNERSHIP_BUSY;
    }
    if (current_owner != claim->owner) {
        return AIRDAP_DAP_OWNERSHIP_NOT_OWNER;
    }
    current_owner = AIRDAP_DAP_OWNER_NONE;
    return AIRDAP_DAP_OWNERSHIP_OK;
}

airdap_dap_ownership_result_t airdap_dap_ownership_operation_begin(
    const airdap_dap_ownership_claim_t *claim,
    airdap_dap_ownership_operation_t *operation)
{
    assert(claim != NULL && operation != NULL);
    ++operation_begin_calls;
    if (operation_active || current_owner != claim->owner) {
        return AIRDAP_DAP_OWNERSHIP_BUSY;
    }
    operation_active = true;
    operation->owner = claim->owner;
    operation->generation = claim->generation;
    operation->active = true;
    if (attach_during_operation_begin) {
        attach_during_operation_begin = false;
        assert(airdap_mode_state_transition(
            AIRDAP_MODE_EVENT_USB_ATTACHED) == AIRDAP_MODE_STATE_OK);
    }
    if (wifi_disconnect_during_operation_begin) {
        wifi_disconnect_during_operation_begin = false;
        assert(airdap_mode_state_transition(
            AIRDAP_MODE_EVENT_WIFI_DISCONNECTED) == AIRDAP_MODE_STATE_OK);
    }
    return AIRDAP_DAP_OWNERSHIP_OK;
}

void airdap_dap_ownership_operation_end(
    airdap_dap_ownership_operation_t *operation)
{
    assert(operation != NULL && operation->active);
    ++operation_end_calls;
    operation_active = false;
    *operation = (airdap_dap_ownership_operation_t) {0};
}

static airdap_mode_snapshot_t snapshot(void)
{
    airdap_mode_snapshot_t state;
    assert(airdap_mode_state_get(&state) == AIRDAP_MODE_STATE_OK);
    return state;
}

static void reset_state(void)
{
    current_owner = AIRDAP_DAP_OWNER_NONE;
    acquire_calls = 0U;
    conditional_revoke_calls = 0U;
    release_calls = 0U;
    operation_begin_calls = 0U;
    operation_end_calls = 0U;
    attach_during_acquire = false;
    attach_during_operation_begin = false;
    wifi_disconnect_during_acquire = false;
    wifi_disconnect_during_operation_begin = false;
    operation_active = false;
    airdap_mode_state_init();
}

static void test_initial_state_and_authentication_boundary(void)
{
    reset_state();

    const airdap_mode_snapshot_t state = snapshot();
    assert(!state.usb_present);
    assert(state.wifi == AIRDAP_WIFI_STOPPED);
    assert(state.provisioning == AIRDAP_PROVISIONING_IDLE);
    assert(state.ota == AIRDAP_OTA_IDLE);
    assert(state.dap_owner == AIRDAP_DAP_OWNER_NONE);

    assert(airdap_mode_state_dap_admission(
        AIRDAP_DAP_OWNER_USB,
        true) == AIRDAP_MODE_DAP_OFFLINE);
    assert(airdap_mode_state_dap_admission(
        AIRDAP_DAP_OWNER_NETWORK,
        false) == AIRDAP_MODE_DAP_UNAUTHENTICATED);
    assert(airdap_mode_state_dap_admission(
        AIRDAP_DAP_OWNER_NETWORK,
        true) == AIRDAP_MODE_DAP_OFFLINE);
}

static void test_usb_priority_and_wifi_reconnect_are_independent(void)
{
    reset_state();

    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_CONNECTING) ==
        AIRDAP_MODE_STATE_OK);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_ONLINE) ==
        AIRDAP_MODE_STATE_OK);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_USB_ATTACHED) ==
        AIRDAP_MODE_STATE_OK);
    assert(snapshot().usb_present);
    assert(snapshot().wifi == AIRDAP_WIFI_ONLINE);
    assert(airdap_mode_state_dap_admission(
        AIRDAP_DAP_OWNER_USB,
        true) == AIRDAP_MODE_DAP_ALLOWED);
    assert(airdap_mode_state_dap_admission(
        AIRDAP_DAP_OWNER_NETWORK,
        true) == AIRDAP_MODE_DAP_BUSY);

    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_DISCONNECTED) ==
        AIRDAP_MODE_STATE_OK);
    assert(snapshot().usb_present);
    assert(airdap_mode_state_dap_admission(
        AIRDAP_DAP_OWNER_USB,
        true) == AIRDAP_MODE_DAP_ALLOWED);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_CONNECTING) ==
        AIRDAP_MODE_STATE_OK);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_ONLINE) ==
        AIRDAP_MODE_STATE_OK);

    current_owner = AIRDAP_DAP_OWNER_USB;
    assert(snapshot().dap_owner == AIRDAP_DAP_OWNER_USB);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_USB_DETACHED) ==
        AIRDAP_MODE_STATE_OK);
    assert(!snapshot().usb_present);
    assert(airdap_mode_state_dap_admission(
        AIRDAP_DAP_OWNER_NETWORK,
        true) == AIRDAP_MODE_DAP_BUSY);

    current_owner = AIRDAP_DAP_OWNER_NONE; /* USB session close releases it. */
    assert(snapshot().dap_owner == AIRDAP_DAP_OWNER_NONE);
    assert(airdap_mode_state_dap_admission(
        AIRDAP_DAP_OWNER_NETWORK,
        true) == AIRDAP_MODE_DAP_ALLOWED);
}

static void test_provisioning_timeout_is_observable(void)
{
    reset_state();

    assert(airdap_mode_state_transition(
        AIRDAP_MODE_EVENT_PROVISIONING_STARTED) == AIRDAP_MODE_STATE_OK);
    assert(snapshot().provisioning == AIRDAP_PROVISIONING_ACTIVE);
    assert(airdap_mode_state_transition(
        AIRDAP_MODE_EVENT_PROVISIONING_TIMED_OUT) == AIRDAP_MODE_STATE_OK);
    assert(snapshot().provisioning == AIRDAP_PROVISIONING_TIMED_OUT);
    assert(airdap_mode_state_transition(
        AIRDAP_MODE_EVENT_PROVISIONING_TIMED_OUT) ==
        AIRDAP_MODE_STATE_INVALID_TRANSITION);

    assert(airdap_mode_state_transition(
        AIRDAP_MODE_EVENT_PROVISIONING_STARTED) == AIRDAP_MODE_STATE_OK);
    assert(airdap_mode_state_transition(
        AIRDAP_MODE_EVENT_PROVISIONING_SUCCEEDED) == AIRDAP_MODE_STATE_OK);
    assert(snapshot().provisioning == AIRDAP_PROVISIONING_SUCCEEDED);
    assert(airdap_mode_state_transition(
        AIRDAP_MODE_EVENT_PROVISIONING_RESET) == AIRDAP_MODE_STATE_OK);
    assert(snapshot().provisioning == AIRDAP_PROVISIONING_IDLE);
}

static void test_usb_acquire_atomically_preempts_network_owner(void)
{
    airdap_dap_ownership_claim_t claim = {0};
    reset_state();

    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_CONNECTING) ==
        AIRDAP_MODE_STATE_OK);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_ONLINE) ==
        AIRDAP_MODE_STATE_OK);
    assert(airdap_mode_state_dap_acquire(
        AIRDAP_DAP_OWNER_NETWORK,
        true,
        &claim) == AIRDAP_MODE_DAP_ALLOWED);
    assert(current_owner == AIRDAP_DAP_OWNER_NETWORK);

    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_USB_ATTACHED) ==
        AIRDAP_MODE_STATE_OK);
    assert(airdap_mode_state_dap_acquire(
        AIRDAP_DAP_OWNER_NETWORK,
        true,
        &claim) == AIRDAP_MODE_DAP_BUSY);
    assert(airdap_mode_state_dap_acquire(
        AIRDAP_DAP_OWNER_USB,
        true,
        &claim) == AIRDAP_MODE_DAP_ALLOWED);
    assert(current_owner == AIRDAP_DAP_OWNER_USB);
    assert(conditional_revoke_calls == 1U);
    assert(acquire_calls == 2U);
}

static void test_network_acquire_rolls_back_if_usb_attaches_mid_transaction(void)
{
    airdap_dap_ownership_claim_t claim = {0};
    reset_state();

    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_CONNECTING) ==
        AIRDAP_MODE_STATE_OK);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_ONLINE) ==
        AIRDAP_MODE_STATE_OK);
    attach_during_acquire = true;
    assert(airdap_mode_state_dap_acquire(
        AIRDAP_DAP_OWNER_NETWORK,
        true,
        &claim) == AIRDAP_MODE_DAP_BUSY);
    assert(snapshot().usb_present);
    assert(current_owner == AIRDAP_DAP_OWNER_NONE);
    assert(acquire_calls == 1U);
    assert(release_calls == 1U);
}

static void test_network_operation_rolls_back_if_usb_attaches_mid_transaction(void)
{
    airdap_dap_ownership_claim_t claim = {0};
    airdap_dap_ownership_operation_t operation = {0};
    reset_state();

    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_CONNECTING) ==
        AIRDAP_MODE_STATE_OK);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_ONLINE) ==
        AIRDAP_MODE_STATE_OK);
    assert(airdap_mode_state_dap_acquire(
        AIRDAP_DAP_OWNER_NETWORK,
        true,
        &claim) == AIRDAP_MODE_DAP_ALLOWED);
    attach_during_operation_begin = true;
    assert(airdap_mode_state_dap_operation_begin(
        AIRDAP_DAP_OWNER_NETWORK,
        true,
        &claim,
        &operation) == AIRDAP_MODE_DAP_BUSY);
    assert(snapshot().usb_present);
    assert(!operation.active);
    assert(operation_begin_calls == 1U);
    assert(operation_end_calls == 1U);
}

static void test_wifi_changes_do_not_rollback_wired_transactions(void)
{
    airdap_dap_ownership_claim_t claim = {0};
    airdap_dap_ownership_operation_t operation = {0};
    reset_state();

    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_USB_ATTACHED) ==
        AIRDAP_MODE_STATE_OK);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_CONNECTING) ==
        AIRDAP_MODE_STATE_OK);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_ONLINE) ==
        AIRDAP_MODE_STATE_OK);
    wifi_disconnect_during_acquire = true;
    assert(airdap_mode_state_dap_acquire(
        AIRDAP_DAP_OWNER_USB,
        true,
        &claim) == AIRDAP_MODE_DAP_ALLOWED);
    assert(snapshot().wifi == AIRDAP_WIFI_DISCONNECTED);
    assert(current_owner == AIRDAP_DAP_OWNER_USB);

    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_CONNECTING) ==
        AIRDAP_MODE_STATE_OK);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_ONLINE) ==
        AIRDAP_MODE_STATE_OK);
    wifi_disconnect_during_operation_begin = true;
    assert(airdap_mode_state_dap_operation_begin(
        AIRDAP_DAP_OWNER_USB,
        true,
        &claim,
        &operation) == AIRDAP_MODE_DAP_ALLOWED);
    assert(operation.active);
    assert(snapshot().wifi == AIRDAP_WIFI_DISCONNECTED);
    airdap_dap_ownership_operation_end(&operation);
}

static void test_ota_blocks_new_debug_owners_until_reset(void)
{
    reset_state();
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_USB_ATTACHED) ==
        AIRDAP_MODE_STATE_OK);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_CONNECTING) ==
        AIRDAP_MODE_STATE_OK);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_ONLINE) ==
        AIRDAP_MODE_STATE_OK);

    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_OTA_STARTED) ==
        AIRDAP_MODE_STATE_OK);
    assert(snapshot().ota == AIRDAP_OTA_RECEIVING);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_OTA_STARTED) ==
        AIRDAP_MODE_STATE_INVALID_TRANSITION);
    assert(airdap_mode_state_dap_admission(
        AIRDAP_DAP_OWNER_USB,
        true) == AIRDAP_MODE_DAP_BUSY);
    assert(airdap_mode_state_dap_admission(
        AIRDAP_DAP_OWNER_DIAGNOSTIC,
        true) == AIRDAP_MODE_DAP_BUSY);

    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_OTA_FAILED) ==
        AIRDAP_MODE_STATE_OK);
    assert(snapshot().ota == AIRDAP_OTA_IDLE);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_OTA_STARTED) ==
        AIRDAP_MODE_STATE_OK);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_OTA_COMMITTED) ==
        AIRDAP_MODE_STATE_OK);
    assert(snapshot().ota == AIRDAP_OTA_READY_TO_REBOOT);
    assert(airdap_mode_state_dap_admission(
        AIRDAP_DAP_OWNER_USB,
        true) == AIRDAP_MODE_DAP_BUSY);

    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_OTA_RESET) ==
        AIRDAP_MODE_STATE_OK);
    assert(snapshot().ota == AIRDAP_OTA_IDLE);
    assert(airdap_mode_state_dap_admission(
        AIRDAP_DAP_OWNER_USB,
        true) == AIRDAP_MODE_DAP_ALLOWED);
}

static void test_invalid_requests_are_rejected(void)
{
    airdap_dap_ownership_claim_t claim = {0};
    airdap_dap_ownership_operation_t operation = {0};
    reset_state();
    assert(airdap_mode_state_get(NULL) == AIRDAP_MODE_STATE_INVALID_ARGUMENT);
    assert(airdap_mode_state_transition((airdap_mode_event_t) 255) ==
        AIRDAP_MODE_STATE_INVALID_ARGUMENT);
    assert(airdap_mode_state_dap_admission(
        AIRDAP_DAP_OWNER_NONE,
        true) == AIRDAP_MODE_DAP_INVALID_ARGUMENT);
    assert(airdap_mode_state_dap_acquire(
        AIRDAP_DAP_OWNER_NETWORK,
        false,
        &claim) == AIRDAP_MODE_DAP_UNAUTHENTICATED);
    assert(acquire_calls == 0U);
    assert(airdap_mode_state_dap_acquire(
        AIRDAP_DAP_OWNER_USB,
        true,
        NULL) == AIRDAP_MODE_DAP_INVALID_ARGUMENT);
    assert(airdap_mode_state_dap_operation_begin(
        AIRDAP_DAP_OWNER_USB,
        true,
        NULL,
        &operation) == AIRDAP_MODE_DAP_INVALID_ARGUMENT);
}

int main(void)
{
    test_initial_state_and_authentication_boundary();
    test_usb_priority_and_wifi_reconnect_are_independent();
    test_provisioning_timeout_is_observable();
    test_usb_acquire_atomically_preempts_network_owner();
    test_network_acquire_rolls_back_if_usb_attaches_mid_transaction();
    test_network_operation_rolls_back_if_usb_attaches_mid_transaction();
    test_wifi_changes_do_not_rollback_wired_transactions();
    test_ota_blocks_new_debug_owners_until_reset();
    test_invalid_requests_are_rejected();
    puts("Mode state tests passed");
    return 0;
}
