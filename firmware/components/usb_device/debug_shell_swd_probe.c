#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airdap_dap_ownership.h"
#include "airdap_debug_shell_swd_probe.h"

static bool is_space(char character)
{
    return character == ' ';
}

static uint8_t parity32(uint32_t value)
{
    uint8_t parity = 0U;

    while (value != 0U) {
        parity ^= (uint8_t) (value & 1U);
        value >>= 1U;
    }
    return parity;
}

static const uint8_t idcode_connect_request[] = {
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0x9EU, 0xE7U,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0x00U,
    0xA5U,
};

static bool probe_cancelled(
    const airdap_debug_shell_swd_backend_t *backend)
{
    return backend->cancelled != NULL && backend->cancelled(backend->context);
}

static airdap_debug_shell_swd_probe_status_t release_diagnostic_owner(
    airdap_debug_shell_swd_probe_status_t status,
    const airdap_dap_ownership_claim_t *claim,
    airdap_dap_ownership_operation_t *operation,
    airdap_debug_shell_swd_probe_result_t *result)
{
    airdap_dap_ownership_operation_end(operation);
    result->release_error = (int) airdap_dap_ownership_release(claim);
    if (status == AIRDAP_DEBUG_SHELL_SWD_PROBE_OK &&
        result->release_error != (int) AIRDAP_DAP_OWNERSHIP_OK) {
        return AIRDAP_DEBUG_SHELL_SWD_PROBE_RELEASE_FAILED;
    }
    return status;
}

static airdap_debug_shell_swd_probe_status_t parse_clock_khz(
    const char *arguments,
    uint32_t *clock_khz)
{
    while (is_space(*arguments)) {
        ++arguments;
    }
    if (*arguments == '\0') {
        *clock_khz = AIRDAP_DEBUG_SHELL_SWD_DEFAULT_CLOCK_KHZ;
        return AIRDAP_DEBUG_SHELL_SWD_PROBE_OK;
    }

    uint32_t value = 0U;
    bool overflow = false;
    const char *cursor = arguments;
    while (*cursor >= '0' && *cursor <= '9') {
        const uint32_t digit = (uint32_t) (*cursor - '0');
        if (value > (UINT32_MAX - digit) / 10U) {
            overflow = true;
        } else if (!overflow) {
            value = value * 10U + digit;
        }
        ++cursor;
    }
    if (cursor == arguments) {
        return AIRDAP_DEBUG_SHELL_SWD_PROBE_USAGE;
    }
    while (is_space(*cursor)) {
        ++cursor;
    }
    if (*cursor != '\0') {
        return AIRDAP_DEBUG_SHELL_SWD_PROBE_USAGE;
    }
    if (overflow ||
        value < AIRDAP_DEBUG_SHELL_SWD_MIN_CLOCK_KHZ ||
        value > AIRDAP_DEBUG_SHELL_SWD_MAX_CLOCK_KHZ) {
        return AIRDAP_DEBUG_SHELL_SWD_PROBE_CLOCK_OUT_OF_RANGE;
    }

    *clock_khz = value;
    return AIRDAP_DEBUG_SHELL_SWD_PROBE_OK;
}

airdap_debug_shell_swd_probe_status_t airdap_debug_shell_swd_probe(
    const char *arguments,
    const airdap_debug_shell_swd_backend_t *backend,
    airdap_debug_shell_swd_probe_result_t *result)
{
    if (result == NULL) {
        return AIRDAP_DEBUG_SHELL_SWD_PROBE_INVALID_BACKEND;
    }
    *result = (airdap_debug_shell_swd_probe_result_t) {0};
    if (arguments == NULL || backend == NULL ||
        backend->set_clock == NULL || backend->write_sequence == NULL ||
        backend->read_sequence == NULL) {
        return AIRDAP_DEBUG_SHELL_SWD_PROBE_INVALID_BACKEND;
    }

    const airdap_debug_shell_swd_probe_status_t parse_status =
        parse_clock_khz(arguments, &result->clock_khz);
    if (parse_status != AIRDAP_DEBUG_SHELL_SWD_PROBE_OK) {
        return parse_status;
    }

    airdap_dap_ownership_claim_t claim = {0};
    const airdap_dap_ownership_result_t acquire_result =
        airdap_dap_ownership_acquire(AIRDAP_DAP_OWNER_DIAGNOSTIC, &claim);
    if (acquire_result == AIRDAP_DAP_OWNERSHIP_BUSY ||
        acquire_result == AIRDAP_DAP_OWNERSHIP_ALREADY_OWNER) {
        return AIRDAP_DEBUG_SHELL_SWD_PROBE_BUSY;
    }
    if (acquire_result != AIRDAP_DAP_OWNERSHIP_OK) {
        result->operation_error = (int) acquire_result;
        return AIRDAP_DEBUG_SHELL_SWD_PROBE_OWNERSHIP_FAILED;
    }

    airdap_dap_ownership_operation_t operation = {0};
    const airdap_dap_ownership_result_t begin_result =
        airdap_dap_ownership_operation_begin(&claim, &operation);
    if (begin_result != AIRDAP_DAP_OWNERSHIP_OK) {
        result->operation_error = (int) begin_result;
        result->release_error = (int) airdap_dap_ownership_release(&claim);
        return AIRDAP_DEBUG_SHELL_SWD_PROBE_OWNERSHIP_FAILED;
    }

    if (probe_cancelled(backend)) {
        return release_diagnostic_owner(
            AIRDAP_DEBUG_SHELL_SWD_PROBE_CANCELLED,
            &claim,
            &operation,
            result);
    }

    result->operation_error = backend->set_clock(
        backend->context,
        result->clock_khz * UINT32_C(1000));
    if (result->operation_error != 0) {
        return release_diagnostic_owner(
            AIRDAP_DEBUG_SHELL_SWD_PROBE_SET_CLOCK_FAILED,
            &claim,
            &operation,
            result);
    }
    if (probe_cancelled(backend)) {
        return release_diagnostic_owner(
            AIRDAP_DEBUG_SHELL_SWD_PROBE_CANCELLED,
            &claim,
            &operation,
            result);
    }

    result->operation_error = backend->write_sequence(
        backend->context,
        idcode_connect_request,
        sizeof(idcode_connect_request) * 8U);
    if (result->operation_error != 0) {
        return release_diagnostic_owner(
            AIRDAP_DEBUG_SHELL_SWD_PROBE_CONNECT_FAILED,
            &claim,
            &operation,
            result);
    }
    if (probe_cancelled(backend)) {
        return release_diagnostic_owner(
            AIRDAP_DEBUG_SHELL_SWD_PROBE_CANCELLED,
            &claim,
            &operation,
            result);
    }

    uint8_t response[5] = {0};
    result->operation_error = backend->read_sequence(
        backend->context,
        response,
        37U);
    if (result->operation_error != 0) {
        return release_diagnostic_owner(
            AIRDAP_DEBUG_SHELL_SWD_PROBE_CONNECT_FAILED,
            &claim,
            &operation,
            result);
    }
    if (probe_cancelled(backend)) {
        return release_diagnostic_owner(
            AIRDAP_DEBUG_SHELL_SWD_PROBE_CANCELLED,
            &claim,
            &operation,
            result);
    }

    for (size_t index = 0U; index < sizeof(response); ++index) {
        result->response_bits |= (uint64_t) response[index] << (index * 8U);
    }
    result->ack = (uint8_t) (result->response_bits & 0x7U);
    result->idcode = (uint32_t) (result->response_bits >> 3U);
    result->received_parity =
        (uint8_t) ((result->response_bits >> 35U) & 1U);
    result->expected_parity = parity32(result->idcode);
    if (result->ack != 1U) {
        return release_diagnostic_owner(
            AIRDAP_DEBUG_SHELL_SWD_PROBE_RESPONSE_INVALID,
            &claim,
            &operation,
            result);
    }
    if (result->received_parity != result->expected_parity) {
        return release_diagnostic_owner(
            AIRDAP_DEBUG_SHELL_SWD_PROBE_PARITY_ERROR,
            &claim,
            &operation,
            result);
    }
    return release_diagnostic_owner(
        AIRDAP_DEBUG_SHELL_SWD_PROBE_OK,
        &claim,
        &operation,
        result);
}
