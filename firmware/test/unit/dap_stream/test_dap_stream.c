#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_dap_stream.h"

typedef struct {
    uint8_t requests[8][AIRDAP_DAP_PACKET_SIZE];
    size_t lengths[8];
    size_t count;
} capture_t;

static void capture_request(
    void *context,
    const uint8_t *request,
    size_t request_length)
{
    capture_t *capture = context;
    assert(capture->count < 8U);
    assert(request_length <= sizeof(capture->requests[0]));
    memcpy(capture->requests[capture->count], request, request_length);
    capture->lengths[capture->count] = request_length;
    ++capture->count;
}

static void test_complete_and_concatenated_requests_are_emitted(void)
{
    airdap_dap_stream_t stream;
    capture_t capture = {0};
    const uint8_t input[] = {
        0x81, 8, 0, 0, 0,
        0x80,
        0x03,
    };

    airdap_dap_stream_init(&stream);
    assert(airdap_dap_stream_feed(
        &stream,
        input,
        sizeof(input),
        100U,
        capture_request,
        &capture) == AIRDAP_DAP_STREAM_OK);
    assert(capture.count == 3U);
    assert(capture.lengths[0] == 5U && capture.requests[0][0] == 0x81U);
    assert(capture.lengths[1] == 1U && capture.requests[1][0] == 0x80U);
    assert(capture.lengths[2] == 1U && capture.requests[2][0] == 0x03U);
    assert(stream.length == 0U);
}

static void test_stale_partial_write_is_cleared_before_next_command(void)
{
    airdap_dap_stream_t stream;
    capture_t capture = {0};
    const uint8_t partial_write[] = {
        0x82,
        0, 0, 0, 0,
        3, 0,
        0xAA,
    };
    const uint8_t query[] = {0x80};
    const int64_t started = 1000;

    airdap_dap_stream_init(&stream);
    assert(airdap_dap_stream_feed(
        &stream,
        partial_write,
        sizeof(partial_write),
        started,
        capture_request,
        &capture) == AIRDAP_DAP_STREAM_OK);
    assert(capture.count == 0U && stream.length == sizeof(partial_write));

    assert(!airdap_dap_stream_expire(
        &stream,
        started + AIRDAP_DAP_STREAM_TIMEOUT_US - 1));
    assert(stream.length == sizeof(partial_write));
    assert(airdap_dap_stream_expire(
        &stream,
        started + AIRDAP_DAP_STREAM_TIMEOUT_US));
    assert(stream.length == 0U);

    assert(airdap_dap_stream_feed(
        &stream,
        query,
        sizeof(query),
        started + AIRDAP_DAP_STREAM_TIMEOUT_US,
        capture_request,
        &capture) == AIRDAP_DAP_STREAM_OK);
    assert(capture.count == 1U && capture.requests[0][0] == 0x80U);
}

static void test_malformed_or_overflowing_input_resets_the_stream(void)
{
    airdap_dap_stream_t stream;
    capture_t capture = {0};
    const uint8_t oversized_write_header[] = {
        0x82,
        0, 0, 0, 0,
        0xF1, 0x01,
    };
    uint8_t overflow[AIRDAP_DAP_BUFFER_SIZE + 1U] = {0};

    airdap_dap_stream_init(&stream);
    assert(airdap_dap_stream_feed(
        &stream,
        oversized_write_header,
        sizeof(oversized_write_header),
        0U,
        capture_request,
        &capture) == AIRDAP_DAP_STREAM_MALFORMED);
    assert(stream.length == 0U && capture.count == 0U);

    assert(airdap_dap_stream_feed(
        &stream,
        overflow,
        sizeof(overflow),
        0U,
        capture_request,
        &capture) == AIRDAP_DAP_STREAM_OVERFLOW);
    assert(stream.length == 0U && capture.count == 0U);
}

int main(void)
{
    test_complete_and_concatenated_requests_are_emitted();
    test_stale_partial_write_is_cleared_before_next_command();
    test_malformed_or_overflowing_input_resets_the_stream();
    puts("DAP stream recovery tests passed");
    return 0;
}
