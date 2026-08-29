#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "airdap_debug_shell_tx_state.h"

static void test_matching_completion(void)
{
    airdap_debug_shell_tx_state_t state;
    airdap_debug_shell_tx_state_init(&state);
    airdap_debug_shell_tx_state_connected(&state);

    const airdap_debug_shell_tx_reservation_t reservation =
        airdap_debug_shell_tx_state_reserve(&state, 21U);
    const airdap_debug_shell_tx_ticket_t ticket =
        airdap_debug_shell_tx_state_commit(&state, reservation, 21U);
    assert(airdap_debug_shell_tx_state_status(&state, ticket) ==
           AIRDAP_DEBUG_SHELL_TX_PENDING);

    airdap_debug_shell_tx_state_completed(&state, 20U);
    assert(airdap_debug_shell_tx_state_status(&state, ticket) ==
           AIRDAP_DEBUG_SHELL_TX_PENDING);
    airdap_debug_shell_tx_state_completed(&state, 1U);
    assert(airdap_debug_shell_tx_state_status(&state, ticket) ==
           AIRDAP_DEBUG_SHELL_TX_COMPLETE);
}

static void test_completion_before_ticket_recording(void)
{
    airdap_debug_shell_tx_state_t state;
    airdap_debug_shell_tx_state_init(&state);
    airdap_debug_shell_tx_state_connected(&state);

    const airdap_debug_shell_tx_reservation_t reservation =
        airdap_debug_shell_tx_state_reserve(&state, 21U);
    airdap_debug_shell_tx_state_completed(&state, 21U);
    const airdap_debug_shell_tx_ticket_t ticket =
        airdap_debug_shell_tx_state_commit(&state, reservation, 21U);

    assert(airdap_debug_shell_tx_state_status(&state, ticket) ==
           AIRDAP_DEBUG_SHELL_TX_COMPLETE);
}

static void test_disconnect_cancels_ticket(void)
{
    airdap_debug_shell_tx_state_t state;
    airdap_debug_shell_tx_state_init(&state);
    airdap_debug_shell_tx_state_connected(&state);
    const airdap_debug_shell_tx_reservation_t reservation =
        airdap_debug_shell_tx_state_reserve(&state, 21U);
    const airdap_debug_shell_tx_ticket_t ticket =
        airdap_debug_shell_tx_state_commit(&state, reservation, 21U);

    airdap_debug_shell_tx_state_disconnected(&state);

    assert(airdap_debug_shell_tx_state_status(&state, ticket) ==
           AIRDAP_DEBUG_SHELL_TX_CANCELLED);
}

static void test_zero_byte_completion_remains_pending(void)
{
    airdap_debug_shell_tx_state_t state;
    airdap_debug_shell_tx_state_init(&state);
    airdap_debug_shell_tx_state_connected(&state);
    const airdap_debug_shell_tx_reservation_t reservation =
        airdap_debug_shell_tx_state_reserve(&state, 21U);
    const airdap_debug_shell_tx_ticket_t ticket =
        airdap_debug_shell_tx_state_commit(&state, reservation, 21U);

    airdap_debug_shell_tx_state_completed(&state, 0U);

    assert(airdap_debug_shell_tx_state_status(&state, ticket) ==
           AIRDAP_DEBUG_SHELL_TX_PENDING);
}

static void test_pending_ticket_provides_timeout_boundary(void)
{
    airdap_debug_shell_tx_state_t state;
    airdap_debug_shell_tx_state_init(&state);
    airdap_debug_shell_tx_state_connected(&state);
    const airdap_debug_shell_tx_reservation_t reservation =
        airdap_debug_shell_tx_state_reserve(&state, 21U);
    const airdap_debug_shell_tx_ticket_t ticket =
        airdap_debug_shell_tx_state_commit(&state, reservation, 21U);

    assert(airdap_debug_shell_tx_state_status(&state, ticket) ==
           AIRDAP_DEBUG_SHELL_TX_PENDING);
}

static void test_reconnect_does_not_revive_cancelled_ticket(void)
{
    airdap_debug_shell_tx_state_t state;
    airdap_debug_shell_tx_state_init(&state);
    airdap_debug_shell_tx_state_connected(&state);
    const airdap_debug_shell_tx_reservation_t old_reservation =
        airdap_debug_shell_tx_state_reserve(&state, 21U);

    airdap_debug_shell_tx_state_disconnected(&state);
    airdap_debug_shell_tx_state_connected(&state);
    const airdap_debug_shell_tx_ticket_t old_ticket =
        airdap_debug_shell_tx_state_commit(&state, old_reservation, 21U);
    airdap_debug_shell_tx_state_completed(&state, 21U);

    assert(airdap_debug_shell_tx_state_status(&state, old_ticket) ==
           AIRDAP_DEBUG_SHELL_TX_CANCELLED);
}

static void test_stale_completion_cannot_complete_new_ticket(void)
{
    airdap_debug_shell_tx_state_t state;
    airdap_debug_shell_tx_state_init(&state);
    airdap_debug_shell_tx_state_connected(&state);
    airdap_debug_shell_tx_state_disconnected(&state);
    airdap_debug_shell_tx_state_connected(&state);

    airdap_debug_shell_tx_state_completed(&state, 21U);
    const airdap_debug_shell_tx_reservation_t reservation =
        airdap_debug_shell_tx_state_reserve(&state, 21U);
    const airdap_debug_shell_tx_ticket_t ticket =
        airdap_debug_shell_tx_state_commit(&state, reservation, 21U);

    assert(airdap_debug_shell_tx_state_status(&state, ticket) ==
           AIRDAP_DEBUG_SHELL_TX_PENDING);
    airdap_debug_shell_tx_state_completed(&state, 21U);
    assert(airdap_debug_shell_tx_state_status(&state, ticket) ==
           AIRDAP_DEBUG_SHELL_TX_COMPLETE);
}

static void test_partial_write_rolls_back_unused_reservation(void)
{
    airdap_debug_shell_tx_state_t state;
    airdap_debug_shell_tx_state_init(&state);
    airdap_debug_shell_tx_state_connected(&state);

    const airdap_debug_shell_tx_reservation_t reservation =
        airdap_debug_shell_tx_state_reserve(&state, 64U);
    airdap_debug_shell_tx_state_completed(&state, 32U);
    const airdap_debug_shell_tx_ticket_t ticket =
        airdap_debug_shell_tx_state_commit(&state, reservation, 32U);

    assert(airdap_debug_shell_tx_state_status(&state, ticket) ==
           AIRDAP_DEBUG_SHELL_TX_COMPLETE);
}

int main(void)
{
    test_matching_completion();
    test_completion_before_ticket_recording();
    test_disconnect_cancels_ticket();
    test_zero_byte_completion_remains_pending();
    test_pending_ticket_provides_timeout_boundary();
    test_reconnect_does_not_revive_cancelled_ticket();
    test_stale_completion_cannot_complete_new_ticket();
    test_partial_write_rolls_back_unused_reservation();
    return 0;
}
