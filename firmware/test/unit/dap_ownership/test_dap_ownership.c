#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "airdap_dap_ownership.h"

typedef enum {
    EVENT_LINE_RESET,
    EVENT_RELEASE_PINS,
} event_t;

typedef struct {
    event_t events[32];
    size_t event_count;
    bool line_reset_succeeds;
    bool release_succeeds;
} fake_backend_t;

static fake_backend_t fake = {
    .line_reset_succeeds = true,
    .release_succeeds = true,
};

static void record_event(fake_backend_t *fake, event_t event)
{
    assert(fake->event_count < sizeof(fake->events) / sizeof(fake->events[0]));
    fake->events[fake->event_count++] = event;
}

static bool fake_line_reset(void *context)
{
    fake_backend_t *fake = context;
    record_event(fake, EVENT_LINE_RESET);
    return fake->line_reset_succeeds;
}

static bool fake_release_pins(void *context)
{
    fake_backend_t *fake = context;
    record_event(fake, EVENT_RELEASE_PINS);
    return fake->release_succeeds;
}

static void test_owner_lifecycle_and_contention(void)
{
    airdap_dap_ownership_claim_t usb_claim = {0};
    airdap_dap_ownership_claim_t repeated_usb_claim = {0};
    airdap_dap_ownership_claim_t network_claim = {0};
    airdap_dap_ownership_claim_t wrong_claim = {0};

    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_NONE);
    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_NONE,
        &usb_claim) ==
        AIRDAP_DAP_OWNERSHIP_INVALID_ARGUMENT);

    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_USB,
        &usb_claim) ==
        AIRDAP_DAP_OWNERSHIP_OK);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_USB);
    assert(fake.event_count == 1U && fake.events[0] == EVENT_LINE_RESET);

    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_USB,
        &repeated_usb_claim) ==
        AIRDAP_DAP_OWNERSHIP_ALREADY_OWNER);
    assert(repeated_usb_claim.owner == usb_claim.owner);
    assert(repeated_usb_claim.generation == usb_claim.generation);
    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_NETWORK,
        &network_claim) ==
        AIRDAP_DAP_OWNERSHIP_BUSY);
    assert(fake.event_count == 1U);

    wrong_claim = usb_claim;
    wrong_claim.owner = AIRDAP_DAP_OWNER_NETWORK;
    assert(airdap_dap_ownership_release(&wrong_claim) ==
        AIRDAP_DAP_OWNERSHIP_NOT_OWNER);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_USB);
    assert(fake.event_count == 1U);

    assert(airdap_dap_ownership_release(&usb_claim) ==
        AIRDAP_DAP_OWNERSHIP_OK);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_NONE);
    assert(fake.event_count == 2U && fake.events[1] == EVENT_RELEASE_PINS);
}

static void test_disconnect_timeout_ota_and_diagnostic_contention(void)
{
    airdap_dap_ownership_claim_t claim = {0};
    airdap_dap_ownership_claim_t competing_claim = {0};

    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_NETWORK,
        &claim) ==
        AIRDAP_DAP_OWNERSHIP_OK);
    assert(airdap_dap_ownership_release(&claim) ==
        AIRDAP_DAP_OWNERSHIP_OK); /* TCP disconnect */

    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_NETWORK,
        &claim) ==
        AIRDAP_DAP_OWNERSHIP_OK);
    assert(airdap_dap_ownership_release(&claim) ==
        AIRDAP_DAP_OWNERSHIP_OK); /* session timeout */

    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_USB,
        &claim) ==
        AIRDAP_DAP_OWNERSHIP_OK);
    assert(airdap_dap_ownership_revoke() == AIRDAP_DAP_OWNERSHIP_OK);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_NONE);

    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_DIAGNOSTIC,
        &claim) ==
        AIRDAP_DAP_OWNERSHIP_OK);
    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_USB,
        &competing_claim) ==
        AIRDAP_DAP_OWNERSHIP_BUSY);
    assert(airdap_dap_ownership_release(&claim) ==
        AIRDAP_DAP_OWNERSHIP_OK);
}

static void test_active_operation_blocks_release_and_revoke(void)
{
    airdap_dap_ownership_operation_t operation = {0};
    airdap_dap_ownership_operation_t wrong_owner_operation = {0};
    airdap_dap_ownership_claim_t usb_claim = {0};
    airdap_dap_ownership_claim_t wrong_claim = {0};
    const size_t events_before = fake.event_count;

    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_USB,
        &usb_claim) ==
        AIRDAP_DAP_OWNERSHIP_OK);
    wrong_claim = usb_claim;
    wrong_claim.owner = AIRDAP_DAP_OWNER_NETWORK;
    assert(airdap_dap_ownership_operation_begin(
        &wrong_claim,
        &wrong_owner_operation) == AIRDAP_DAP_OWNERSHIP_NOT_OWNER);
    assert(airdap_dap_ownership_operation_begin(
        &usb_claim,
        &operation) == AIRDAP_DAP_OWNERSHIP_OK);

    assert(airdap_dap_ownership_release(&usb_claim) ==
        AIRDAP_DAP_OWNERSHIP_BUSY);
    assert(airdap_dap_ownership_revoke() == AIRDAP_DAP_OWNERSHIP_BUSY);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_USB);
    assert(fake.event_count == events_before + 1U);

    airdap_dap_ownership_operation_end(&operation);
    assert(airdap_dap_ownership_revoke() == AIRDAP_DAP_OWNERSHIP_OK);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_NONE);
    assert(fake.event_count == events_before + 2U);
    airdap_dap_ownership_operation_end(&wrong_owner_operation);
}

static void test_stale_claim_cannot_cross_same_owner_reacquire(void)
{
    airdap_dap_ownership_claim_t old_claim = {0};
    airdap_dap_ownership_claim_t new_claim = {0};
    airdap_dap_ownership_operation_t operation = {0};

    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_USB,
        &old_claim) == AIRDAP_DAP_OWNERSHIP_OK);
    assert(airdap_dap_ownership_release(&old_claim) ==
        AIRDAP_DAP_OWNERSHIP_OK);
    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_USB,
        &new_claim) == AIRDAP_DAP_OWNERSHIP_OK);
    assert(new_claim.generation != old_claim.generation);

    assert(airdap_dap_ownership_operation_begin(&old_claim, &operation) ==
        AIRDAP_DAP_OWNERSHIP_NOT_OWNER);
    assert(airdap_dap_ownership_operation_begin(&new_claim, &operation) ==
        AIRDAP_DAP_OWNERSHIP_OK);
    airdap_dap_ownership_operation_end(&operation);
    assert(airdap_dap_ownership_release(&new_claim) ==
        AIRDAP_DAP_OWNERSHIP_OK);
}

static void test_backend_failures_leave_the_bus_unowned(void)
{
    const size_t events_before = fake.event_count;
    airdap_dap_ownership_claim_t claim = {0};

    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_NONE);
    fake.line_reset_succeeds = false;
    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_USB,
        &claim) ==
        AIRDAP_DAP_OWNERSHIP_OFFLINE);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_NONE);
    assert(fake.event_count == events_before + 2U);
    assert(fake.events[events_before] == EVENT_LINE_RESET);
    assert(fake.events[events_before + 1U] == EVENT_RELEASE_PINS);

    fake.line_reset_succeeds = true;
    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_DIAGNOSTIC,
        &claim) ==
        AIRDAP_DAP_OWNERSHIP_OK);
    fake.release_succeeds = false;
    assert(airdap_dap_ownership_release(&claim) ==
        AIRDAP_DAP_OWNERSHIP_OFFLINE);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_NONE);
    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_USB,
        &claim) ==
        AIRDAP_DAP_OWNERSHIP_OFFLINE);
}

int main(void)
{
    const airdap_dap_ownership_backend_t backend = {
        .context = &fake,
        .line_reset = fake_line_reset,
        .release_pins = fake_release_pins,
    };

    assert(airdap_dap_ownership_initialize(NULL) ==
        AIRDAP_DAP_OWNERSHIP_INVALID_ARGUMENT);
    assert(airdap_dap_ownership_initialize(&backend) ==
        AIRDAP_DAP_OWNERSHIP_OK);
    assert(airdap_dap_ownership_initialize(&backend) ==
        AIRDAP_DAP_OWNERSHIP_INVALID_STATE);
    test_owner_lifecycle_and_contention();
    test_disconnect_timeout_ota_and_diagnostic_contention();
    test_active_operation_blocks_release_and_revoke();
    test_stale_claim_cannot_cross_same_owner_reacquire();
    test_backend_failures_leave_the_bus_unowned();
    puts("DAP ownership tests passed");
    return 0;
}
