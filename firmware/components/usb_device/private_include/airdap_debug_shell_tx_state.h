#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t generation;
    uint64_t accepted_bytes;
    uint64_t completed_bytes;
    bool connected;
} airdap_debug_shell_tx_state_t;

typedef struct {
    uint64_t generation;
    uint64_t reserved_bytes;
    bool valid;
} airdap_debug_shell_tx_reservation_t;

typedef struct {
    uint64_t generation;
    uint64_t target_bytes;
    bool valid;
} airdap_debug_shell_tx_ticket_t;

typedef enum {
    AIRDAP_DEBUG_SHELL_TX_PENDING,
    AIRDAP_DEBUG_SHELL_TX_COMPLETE,
    AIRDAP_DEBUG_SHELL_TX_CANCELLED,
} airdap_debug_shell_tx_status_t;

void airdap_debug_shell_tx_state_init(airdap_debug_shell_tx_state_t *state);
void airdap_debug_shell_tx_state_connected(airdap_debug_shell_tx_state_t *state);
void airdap_debug_shell_tx_state_disconnected(airdap_debug_shell_tx_state_t *state);
airdap_debug_shell_tx_reservation_t airdap_debug_shell_tx_state_reserve(
    airdap_debug_shell_tx_state_t *state,
    size_t requested_bytes);
airdap_debug_shell_tx_ticket_t airdap_debug_shell_tx_state_commit(
    airdap_debug_shell_tx_state_t *state,
    airdap_debug_shell_tx_reservation_t reservation,
    size_t accepted_bytes);
void airdap_debug_shell_tx_state_completed(
    airdap_debug_shell_tx_state_t *state,
    size_t completed_bytes);
airdap_debug_shell_tx_status_t airdap_debug_shell_tx_state_status(
    const airdap_debug_shell_tx_state_t *state,
    airdap_debug_shell_tx_ticket_t ticket);
