#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "airdap_dap_stream.h"

void airdap_dap_stream_init(airdap_dap_stream_t *stream)
{
    if (stream != NULL) {
        memset(stream, 0, sizeof(*stream));
    }
}

bool airdap_dap_stream_expire(
    airdap_dap_stream_t *stream,
    int64_t now_us)
{
    if (stream == NULL || stream->length == 0U ||
        now_us < stream->last_activity_us ||
        now_us - stream->last_activity_us < AIRDAP_DAP_STREAM_TIMEOUT_US) {
        return false;
    }

    airdap_dap_stream_init(stream);
    return true;
}

airdap_dap_stream_result_t airdap_dap_stream_feed(
    airdap_dap_stream_t *stream,
    const uint8_t *data,
    size_t data_length,
    int64_t now_us,
    airdap_dap_stream_request_fn request_callback,
    void *request_context)
{
    if (stream == NULL || request_callback == NULL ||
        (data == NULL && data_length > 0U)) {
        return AIRDAP_DAP_STREAM_MALFORMED;
    }
    if (data_length > sizeof(stream->data) - stream->length) {
        airdap_dap_stream_init(stream);
        return AIRDAP_DAP_STREAM_OVERFLOW;
    }

    if (data_length > 0U) {
        memcpy(stream->data + stream->length, data, data_length);
    }
    stream->length += data_length;

    size_t offset = 0U;
    while (offset < stream->length) {
        const size_t request_length = airdap_dap_request_size(
            stream->data + offset,
            stream->length - offset);
        if (request_length == 0U) {
            break;
        }
        if (request_length == SIZE_MAX ||
            request_length > AIRDAP_DAP_PACKET_SIZE) {
            airdap_dap_stream_init(stream);
            return AIRDAP_DAP_STREAM_MALFORMED;
        }

        request_callback(
            request_context,
            stream->data + offset,
            request_length);
        offset += request_length;
    }

    if (offset > 0U) {
        stream->length -= offset;
        memmove(stream->data, stream->data + offset, stream->length);
    }
    stream->last_activity_us = stream->length > 0U ? now_us : 0;
    return AIRDAP_DAP_STREAM_OK;
}
