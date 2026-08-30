#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    AIRDAP_DEBUG_SHELL_SWD_DEFAULT_CLOCK_KHZ = 100,
    AIRDAP_DEBUG_SHELL_SWD_MIN_CLOCK_KHZ = 100,
    AIRDAP_DEBUG_SHELL_SWD_MAX_CLOCK_KHZ = 10000,
};

typedef enum {
    AIRDAP_DEBUG_SHELL_SWD_PROBE_OK = 0,
    AIRDAP_DEBUG_SHELL_SWD_PROBE_USAGE,
    AIRDAP_DEBUG_SHELL_SWD_PROBE_CLOCK_OUT_OF_RANGE,
    AIRDAP_DEBUG_SHELL_SWD_PROBE_INVALID_BACKEND,
    AIRDAP_DEBUG_SHELL_SWD_PROBE_BUSY,
    AIRDAP_DEBUG_SHELL_SWD_PROBE_OWNERSHIP_FAILED,
    AIRDAP_DEBUG_SHELL_SWD_PROBE_CANCELLED,
    AIRDAP_DEBUG_SHELL_SWD_PROBE_SET_CLOCK_FAILED,
    AIRDAP_DEBUG_SHELL_SWD_PROBE_CONNECT_FAILED,
    AIRDAP_DEBUG_SHELL_SWD_PROBE_RESPONSE_INVALID,
    AIRDAP_DEBUG_SHELL_SWD_PROBE_PARITY_ERROR,
    AIRDAP_DEBUG_SHELL_SWD_PROBE_RELEASE_FAILED,
} airdap_debug_shell_swd_probe_status_t;

typedef struct {
    void *context;
    int (*set_clock)(void *context, uint32_t clock_hz);
    int (*write_sequence)(
        void *context,
        const uint8_t *data,
        size_t bit_count);
    int (*read_sequence)(void *context, uint8_t *data, size_t bit_count);
    bool (*cancelled)(void *context);
} airdap_debug_shell_swd_backend_t;

typedef struct {
    uint32_t clock_khz;
    uint32_t idcode;
    uint64_t response_bits;
    uint8_t ack;
    uint8_t received_parity;
    uint8_t expected_parity;
    int operation_error;
    int release_error;
} airdap_debug_shell_swd_probe_result_t;

airdap_debug_shell_swd_probe_status_t airdap_debug_shell_swd_probe(
    const char *arguments,
    const airdap_debug_shell_swd_backend_t *backend,
    airdap_debug_shell_swd_probe_result_t *result);
