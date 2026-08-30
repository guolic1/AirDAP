#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airdap_dap_protocol.h"

enum {
    AIRDAP_DAP_STREAM_TIMEOUT_US = 250000,
};

typedef enum {
    AIRDAP_DAP_STREAM_OK,
    AIRDAP_DAP_STREAM_MALFORMED,
    AIRDAP_DAP_STREAM_OVERFLOW,
} airdap_dap_stream_result_t;

typedef void (*airdap_dap_stream_request_fn)(
    void *context,
    const uint8_t *request,
    size_t request_length);

typedef struct {
    size_t length;
    int64_t last_activity_us;
    uint8_t data[AIRDAP_DAP_BUFFER_SIZE];
} airdap_dap_stream_t;

void airdap_dap_stream_init(airdap_dap_stream_t *stream);

bool airdap_dap_stream_expire(
    airdap_dap_stream_t *stream,
    int64_t now_us);

airdap_dap_stream_result_t airdap_dap_stream_feed(
    airdap_dap_stream_t *stream,
    const uint8_t *data,
    size_t data_length,
    int64_t now_us,
    airdap_dap_stream_request_fn request_callback,
    void *request_context);
