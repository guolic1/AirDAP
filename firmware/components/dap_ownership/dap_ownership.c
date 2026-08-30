#include <limits.h>
#include <stddef.h>
#include <stdatomic.h>

#include "airdap_dap_ownership.h"

enum {
    OWNERSHIP_STATE_TRANSITION = AIRDAP_DAP_OWNER_DIAGNOSTIC + 1,
    OWNERSHIP_STATE_OFFLINE,
    OWNERSHIP_STATE_UNINITIALIZED,
    OWNERSHIP_CONTROL_STATE_MASK = 0x7U,
    OWNERSHIP_CONTROL_ACTIVE = 1U << 3,
    OWNERSHIP_CONTROL_GENERATION_SHIFT = 4,
};

#define OWNERSHIP_CONTROL_GENERATION_MAX \
    (UINT_MAX >> OWNERSHIP_CONTROL_GENERATION_SHIFT)

static airdap_dap_ownership_backend_t ownership_backend;
static atomic_uint ownership_control = OWNERSHIP_STATE_UNINITIALIZED;

static bool is_owner(unsigned int state)
{
    return state >= AIRDAP_DAP_OWNER_USB &&
        state <= AIRDAP_DAP_OWNER_DIAGNOSTIC;
}

static unsigned int control_state(unsigned int control)
{
    return control & OWNERSHIP_CONTROL_STATE_MASK;
}

static unsigned int control_generation(unsigned int control)
{
    return control >> OWNERSHIP_CONTROL_GENERATION_SHIFT;
}

static unsigned int make_control(
    unsigned int generation,
    unsigned int state)
{
    return (generation << OWNERSHIP_CONTROL_GENERATION_SHIFT) | state;
}

static bool valid_claim(const airdap_dap_ownership_claim_t *claim)
{
    return claim != NULL && is_owner((unsigned int) claim->owner) &&
        claim->generation > 0U &&
        claim->generation <= OWNERSHIP_CONTROL_GENERATION_MAX;
}

static airdap_dap_ownership_result_t claim_failure_result(
    unsigned int control,
    const airdap_dap_ownership_claim_t *claim)
{
    const unsigned int state = control_state(control);
    if (state == OWNERSHIP_STATE_OFFLINE) {
        return AIRDAP_DAP_OWNERSHIP_OFFLINE;
    }
    if (state == OWNERSHIP_STATE_UNINITIALIZED) {
        return AIRDAP_DAP_OWNERSHIP_INVALID_STATE;
    }
    if (state == OWNERSHIP_STATE_TRANSITION ||
        (state == (unsigned int) claim->owner &&
         control_generation(control) == claim->generation &&
         (control & OWNERSHIP_CONTROL_ACTIVE) != 0U)) {
        return AIRDAP_DAP_OWNERSHIP_BUSY;
    }
    return AIRDAP_DAP_OWNERSHIP_NOT_OWNER;
}

static airdap_dap_ownership_result_t release_pins(unsigned int generation)
{
    const bool released = ownership_backend.release_pins(
        ownership_backend.context);
    atomic_store(
        &ownership_control,
        make_control(
            generation,
            released ? AIRDAP_DAP_OWNER_NONE : OWNERSHIP_STATE_OFFLINE));
    return released
        ? AIRDAP_DAP_OWNERSHIP_OK
        : AIRDAP_DAP_OWNERSHIP_OFFLINE;
}

airdap_dap_ownership_result_t airdap_dap_ownership_initialize(
    const airdap_dap_ownership_backend_t *backend)
{
    if (backend == NULL || backend->line_reset == NULL ||
        backend->release_pins == NULL) {
        return AIRDAP_DAP_OWNERSHIP_INVALID_ARGUMENT;
    }

    unsigned int expected = OWNERSHIP_STATE_UNINITIALIZED;
    if (!atomic_compare_exchange_strong(
        &ownership_control,
        &expected,
        OWNERSHIP_STATE_TRANSITION)) {
        return AIRDAP_DAP_OWNERSHIP_INVALID_STATE;
    }

    ownership_backend = *backend;
    atomic_store(&ownership_control, AIRDAP_DAP_OWNER_NONE);
    return AIRDAP_DAP_OWNERSHIP_OK;
}

airdap_dap_owner_t airdap_dap_ownership_current(void)
{
    const unsigned int state = control_state(atomic_load(&ownership_control));
    return is_owner(state) ? (airdap_dap_owner_t) state : AIRDAP_DAP_OWNER_NONE;
}

airdap_dap_ownership_result_t airdap_dap_ownership_acquire(
    airdap_dap_owner_t owner,
    airdap_dap_ownership_claim_t *claim)
{
    if (claim == NULL || !is_owner((unsigned int) owner)) {
        return AIRDAP_DAP_OWNERSHIP_INVALID_ARGUMENT;
    }
    *claim = (airdap_dap_ownership_claim_t) {0};

    unsigned int expected = atomic_load(&ownership_control);
    const unsigned int state = control_state(expected);
    if (state == (unsigned int) owner) {
        claim->owner = owner;
        claim->generation = control_generation(expected);
        return AIRDAP_DAP_OWNERSHIP_ALREADY_OWNER;
    }
    if (state == OWNERSHIP_STATE_OFFLINE) {
        return AIRDAP_DAP_OWNERSHIP_OFFLINE;
    }
    if (state == OWNERSHIP_STATE_UNINITIALIZED) {
        return AIRDAP_DAP_OWNERSHIP_INVALID_STATE;
    }
    if (state != AIRDAP_DAP_OWNER_NONE) {
        return AIRDAP_DAP_OWNERSHIP_BUSY;
    }

    const unsigned int generation = control_generation(expected);
    if (!atomic_compare_exchange_strong(
        &ownership_control,
        &expected,
        make_control(generation, OWNERSHIP_STATE_TRANSITION))) {
        const unsigned int observed_state = control_state(expected);
        if (observed_state == (unsigned int) owner) {
            claim->owner = owner;
            claim->generation = control_generation(expected);
            return AIRDAP_DAP_OWNERSHIP_ALREADY_OWNER;
        }
        if (observed_state == OWNERSHIP_STATE_OFFLINE) {
            return AIRDAP_DAP_OWNERSHIP_OFFLINE;
        }
        if (observed_state == OWNERSHIP_STATE_UNINITIALIZED) {
            return AIRDAP_DAP_OWNERSHIP_INVALID_STATE;
        }
        return AIRDAP_DAP_OWNERSHIP_BUSY;
    }

    if (generation == OWNERSHIP_CONTROL_GENERATION_MAX) {
        atomic_store(
            &ownership_control,
            make_control(generation, OWNERSHIP_STATE_OFFLINE));
        return AIRDAP_DAP_OWNERSHIP_OFFLINE;
    }

    if (!ownership_backend.line_reset(ownership_backend.context)) {
        (void) release_pins(generation);
        return AIRDAP_DAP_OWNERSHIP_OFFLINE;
    }

    const unsigned int next_generation = generation + 1U;
    atomic_store(
        &ownership_control,
        make_control(next_generation, (unsigned int) owner));
    claim->owner = owner;
    claim->generation = next_generation;
    return AIRDAP_DAP_OWNERSHIP_OK;
}

airdap_dap_ownership_result_t airdap_dap_ownership_release(
    const airdap_dap_ownership_claim_t *claim)
{
    if (!valid_claim(claim)) {
        return AIRDAP_DAP_OWNERSHIP_INVALID_ARGUMENT;
    }

    unsigned int expected = make_control(
        claim->generation,
        (unsigned int) claim->owner);
    if (!atomic_compare_exchange_strong(
        &ownership_control,
        &expected,
        make_control(claim->generation, OWNERSHIP_STATE_TRANSITION))) {
        return claim_failure_result(expected, claim);
    }

    return release_pins(claim->generation);
}

airdap_dap_ownership_result_t airdap_dap_ownership_operation_begin(
    const airdap_dap_ownership_claim_t *claim,
    airdap_dap_ownership_operation_t *operation)
{
    if (!valid_claim(claim) || operation == NULL) {
        return AIRDAP_DAP_OWNERSHIP_INVALID_ARGUMENT;
    }
    if (operation->active) {
        return AIRDAP_DAP_OWNERSHIP_INVALID_STATE;
    }

    unsigned int expected = make_control(
        claim->generation,
        (unsigned int) claim->owner);
    if (!atomic_compare_exchange_strong(
        &ownership_control,
        &expected,
        expected | OWNERSHIP_CONTROL_ACTIVE)) {
        return claim_failure_result(expected, claim);
    }

    operation->owner = claim->owner;
    operation->generation = claim->generation;
    operation->active = true;
    return AIRDAP_DAP_OWNERSHIP_OK;
}

void airdap_dap_ownership_operation_end(
    airdap_dap_ownership_operation_t *operation)
{
    if (operation == NULL || !operation->active) {
        return;
    }

    unsigned int expected = make_control(
        operation->generation,
        (unsigned int) operation->owner) | OWNERSHIP_CONTROL_ACTIVE;
    (void) atomic_compare_exchange_strong(
        &ownership_control,
        &expected,
        expected & ~OWNERSHIP_CONTROL_ACTIVE);
    operation->active = false;
    operation->owner = AIRDAP_DAP_OWNER_NONE;
    operation->generation = 0U;
}

airdap_dap_ownership_result_t airdap_dap_ownership_revoke(void)
{
    unsigned int current = atomic_load(&ownership_control);
    const unsigned int state = control_state(current);
    if (state == AIRDAP_DAP_OWNER_NONE) {
        return AIRDAP_DAP_OWNERSHIP_OK;
    }
    if (state == OWNERSHIP_STATE_OFFLINE) {
        return AIRDAP_DAP_OWNERSHIP_OFFLINE;
    }
    if (state == OWNERSHIP_STATE_UNINITIALIZED) {
        return AIRDAP_DAP_OWNERSHIP_INVALID_STATE;
    }
    if (!is_owner(state) || (current & OWNERSHIP_CONTROL_ACTIVE) != 0U) {
        return AIRDAP_DAP_OWNERSHIP_BUSY;
    }

    const unsigned int generation = control_generation(current);
    if (!atomic_compare_exchange_strong(
        &ownership_control,
        &current,
        make_control(generation, OWNERSHIP_STATE_TRANSITION))) {
        const unsigned int observed_state = control_state(current);
        if (observed_state == AIRDAP_DAP_OWNER_NONE) {
            return AIRDAP_DAP_OWNERSHIP_OK;
        }
        if (observed_state == OWNERSHIP_STATE_OFFLINE) {
            return AIRDAP_DAP_OWNERSHIP_OFFLINE;
        }
        if (observed_state == OWNERSHIP_STATE_UNINITIALIZED) {
            return AIRDAP_DAP_OWNERSHIP_INVALID_STATE;
        }
        return AIRDAP_DAP_OWNERSHIP_BUSY;
    }
    return release_pins(generation);
}
