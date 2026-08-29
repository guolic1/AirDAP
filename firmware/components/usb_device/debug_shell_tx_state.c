#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "airdap_debug_shell_tx_state.h"

void airdap_debug_shell_tx_state_init(airdap_debug_shell_tx_state_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

void airdap_debug_shell_tx_state_connected(airdap_debug_shell_tx_state_t *state)
{
    if (state == NULL || state->connected) {
        return;
    }

    ++state->generation;
    if (state->generation == 0U) {
        ++state->generation;
    }
    state->accepted_bytes = 0U;
    state->completed_bytes = 0U;
    state->connected = true;
}

void airdap_debug_shell_tx_state_disconnected(airdap_debug_shell_tx_state_t *state)
{
    if (state == NULL || !state->connected) {
        return;
    }

    state->connected = false;
    ++state->generation;
    if (state->generation == 0U) {
        ++state->generation;
    }
}

airdap_debug_shell_tx_reservation_t airdap_debug_shell_tx_state_reserve(
    airdap_debug_shell_tx_state_t *state,
    size_t requested_bytes)
{
    airdap_debug_shell_tx_reservation_t reservation = {0};
    if (state == NULL || !state->connected || requested_bytes == 0U) {
        return reservation;
    }

    state->accepted_bytes += requested_bytes;
    reservation.generation = state->generation;
    reservation.reserved_bytes = requested_bytes;
    reservation.valid = true;
    return reservation;
}

airdap_debug_shell_tx_ticket_t airdap_debug_shell_tx_state_commit(
    airdap_debug_shell_tx_state_t *state,
    airdap_debug_shell_tx_reservation_t reservation,
    size_t accepted_bytes)
{
    airdap_debug_shell_tx_ticket_t ticket = {0};
    if (state == NULL || !state->connected || !reservation.valid ||
        reservation.generation != state->generation ||
        accepted_bytes > reservation.reserved_bytes ||
        state->accepted_bytes < reservation.reserved_bytes) {
        return ticket;
    }

    state->accepted_bytes -= reservation.reserved_bytes - accepted_bytes;
    ticket.generation = reservation.generation;
    ticket.target_bytes = state->accepted_bytes;
    ticket.valid = true;
    return ticket;
}

void airdap_debug_shell_tx_state_completed(
    airdap_debug_shell_tx_state_t *state,
    size_t completed_bytes)
{
    if (state != NULL && state->connected && completed_bytes > 0U) {
        const uint64_t outstanding =
            state->accepted_bytes > state->completed_bytes
            ? state->accepted_bytes - state->completed_bytes
            : 0U;
        state->completed_bytes += completed_bytes < outstanding
            ? completed_bytes
            : outstanding;
    }
}

airdap_debug_shell_tx_status_t airdap_debug_shell_tx_state_status(
    const airdap_debug_shell_tx_state_t *state,
    airdap_debug_shell_tx_ticket_t ticket)
{
    if (state == NULL || !ticket.valid || !state->connected ||
        ticket.generation != state->generation) {
        return AIRDAP_DEBUG_SHELL_TX_CANCELLED;
    }
    return state->completed_bytes >= ticket.target_bytes
        ? AIRDAP_DEBUG_SHELL_TX_COMPLETE
        : AIRDAP_DEBUG_SHELL_TX_PENDING;
}
