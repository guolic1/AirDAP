#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "airdap_frame.h"

static uint16_t read_u16_be(const uint8_t *data)
{
    return ((uint16_t) data[0] << 8U) | (uint16_t) data[1];
}

static uint32_t read_u32_be(const uint8_t *data)
{
    return ((uint32_t) data[0] << 24U) |
        ((uint32_t) data[1] << 16U) |
        ((uint32_t) data[2] << 8U) |
        (uint32_t) data[3];
}

static void write_u16_be(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t) (value >> 8U);
    data[1] = (uint8_t) value;
}

static void write_u32_be(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t) (value >> 24U);
    data[1] = (uint8_t) (value >> 16U);
    data[2] = (uint8_t) (value >> 8U);
    data[3] = (uint8_t) value;
}

static bool valid_message_type(uint8_t type)
{
    return type >= AIRDAP_FRAME_TYPE_HELLO &&
        type <= AIRDAP_FRAME_TYPE_ERROR;
}

static bool valid_error_code(airdap_frame_error_code_t error_code)
{
    switch (error_code) {
    case AIRDAP_FRAME_ERROR_TRUNCATED:
    case AIRDAP_FRAME_ERROR_PAYLOAD_TOO_LARGE:
    case AIRDAP_FRAME_ERROR_UNSUPPORTED_VERSION:
    case AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE:
    case AIRDAP_FRAME_ERROR_INVALID_MAGIC:
    case AIRDAP_FRAME_ERROR_INVALID_FLAGS:
    case AIRDAP_FRAME_ERROR_INVALID_RESERVED:
    case AIRDAP_FRAME_ERROR_SESSION_MISMATCH:
    case AIRDAP_FRAME_ERROR_SEQUENCE_DUPLICATE:
    case AIRDAP_FRAME_ERROR_SEQUENCE_STALE:
    case AIRDAP_FRAME_ERROR_SEQUENCE_OUT_OF_ORDER:
    case AIRDAP_FRAME_ERROR_RESPONSE_MISMATCH:
    case AIRDAP_FRAME_ERROR_BUSY:
    case AIRDAP_FRAME_ERROR_UNAUTHENTICATED:
    case AIRDAP_FRAME_ERROR_TIMEOUT:
    case AIRDAP_FRAME_ERROR_INTERNAL:
        return true;
    case AIRDAP_FRAME_ERROR_NONE:
    default:
        return false;
    }
}

static bool request_message_type(uint8_t type)
{
    switch (type) {
    case AIRDAP_FRAME_TYPE_HELLO:
    case AIRDAP_FRAME_TYPE_AUTH:
    case AIRDAP_FRAME_TYPE_DAP_REQUEST:
    case AIRDAP_FRAME_TYPE_CONTROL_REQUEST:
    case AIRDAP_FRAME_TYPE_KEEPALIVE:
        return true;
    default:
        return false;
    }
}

static airdap_frame_error_code_t validate_header(
    const airdap_frame_header_t *header)
{
    if (header == NULL) {
        return AIRDAP_FRAME_ERROR_INTERNAL;
    }
    if (header->magic != AIRDAP_FRAME_MAGIC) {
        return AIRDAP_FRAME_ERROR_INVALID_MAGIC;
    }
    if (header->version != AIRDAP_FRAME_PROTOCOL_VERSION) {
        return AIRDAP_FRAME_ERROR_UNSUPPORTED_VERSION;
    }
    if (!valid_message_type(header->type)) {
        return AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE;
    }
    if (header->flags != 0U) {
        return AIRDAP_FRAME_ERROR_INVALID_FLAGS;
    }
    if (header->reserved != 0U) {
        return AIRDAP_FRAME_ERROR_INVALID_RESERVED;
    }
    if (header->session_id == 0U) {
        return AIRDAP_FRAME_ERROR_SESSION_MISMATCH;
    }
    if (header->sequence == 0U) {
        return AIRDAP_FRAME_ERROR_SEQUENCE_OUT_OF_ORDER;
    }
    if (header->payload_length > AIRDAP_FRAME_MAX_PAYLOAD_SIZE) {
        return AIRDAP_FRAME_ERROR_PAYLOAD_TOO_LARGE;
    }
    if (header->type == AIRDAP_FRAME_TYPE_DAP_REQUEST &&
        header->payload_length >
            AIRDAP_FRAME_DAP_REQUEST_MAX_PAYLOAD_SIZE) {
        return AIRDAP_FRAME_ERROR_PAYLOAD_TOO_LARGE;
    }
    return AIRDAP_FRAME_ERROR_NONE;
}

airdap_frame_decode_status_t airdap_frame_decode(
    const uint8_t *input,
    size_t input_size,
    airdap_frame_header_t *header,
    const uint8_t **payload,
    size_t *frame_size,
    airdap_frame_error_code_t *error_code)
{
    if (header == NULL || payload == NULL || frame_size == NULL ||
        error_code == NULL) {
        return AIRDAP_FRAME_DECODE_INVALID_FRAME;
    }

    *header = (airdap_frame_header_t) {0};
    *payload = NULL;
    *frame_size = 0U;
    *error_code = AIRDAP_FRAME_ERROR_INTERNAL;

    if (input == NULL && input_size > 0U) {
        return AIRDAP_FRAME_DECODE_INVALID_FRAME;
    }
    if (input_size < AIRDAP_FRAME_HEADER_SIZE) {
        *error_code = AIRDAP_FRAME_ERROR_TRUNCATED;
        return AIRDAP_FRAME_DECODE_NEED_MORE_DATA;
    }

    const airdap_frame_header_t candidate = {
        .magic = read_u32_be(input),
        .version = input[4],
        .type = input[5],
        .flags = read_u16_be(input + 6U),
        .session_id = read_u32_be(input + 8U),
        .sequence = read_u32_be(input + 12U),
        .payload_length = read_u16_be(input + 16U),
        .reserved = read_u16_be(input + 18U),
    };
    const airdap_frame_error_code_t validation_error =
        validate_header(&candidate);
    if (validation_error != AIRDAP_FRAME_ERROR_NONE) {
        *error_code = validation_error;
        return AIRDAP_FRAME_DECODE_INVALID_FRAME;
    }

    const size_t required_size =
        AIRDAP_FRAME_HEADER_SIZE + (size_t) candidate.payload_length;
    if (input_size < required_size) {
        *error_code = AIRDAP_FRAME_ERROR_TRUNCATED;
        return AIRDAP_FRAME_DECODE_NEED_MORE_DATA;
    }

    *header = candidate;
    *payload = input + AIRDAP_FRAME_HEADER_SIZE;
    *frame_size = required_size;
    *error_code = AIRDAP_FRAME_ERROR_NONE;
    return AIRDAP_FRAME_DECODE_OK;
}

airdap_frame_error_code_t airdap_frame_encode(
    const airdap_frame_header_t *header,
    const uint8_t *payload,
    uint8_t *output,
    size_t output_capacity,
    size_t *encoded_size)
{
    if (encoded_size == NULL) {
        return AIRDAP_FRAME_ERROR_INTERNAL;
    }
    *encoded_size = 0U;

    const airdap_frame_error_code_t validation_error =
        validate_header(header);
    if (validation_error != AIRDAP_FRAME_ERROR_NONE) {
        return validation_error;
    }
    if (header->payload_length > 0U && payload == NULL) {
        return AIRDAP_FRAME_ERROR_INTERNAL;
    }

    const size_t required_size =
        AIRDAP_FRAME_HEADER_SIZE + (size_t) header->payload_length;
    *encoded_size = required_size;
    if (output_capacity < required_size) {
        return AIRDAP_FRAME_ERROR_TRUNCATED;
    }
    if (output == NULL) {
        return AIRDAP_FRAME_ERROR_INTERNAL;
    }

    write_u32_be(output, header->magic);
    output[4] = header->version;
    output[5] = header->type;
    write_u16_be(output + 6U, header->flags);
    write_u32_be(output + 8U, header->session_id);
    write_u32_be(output + 12U, header->sequence);
    write_u16_be(output + 16U, header->payload_length);
    write_u16_be(output + 18U, header->reserved);
    if (header->payload_length > 0U) {
        memcpy(
            output + AIRDAP_FRAME_HEADER_SIZE,
            payload,
            header->payload_length);
    }
    return AIRDAP_FRAME_ERROR_NONE;
}

bool airdap_frame_error_code_encode(
    airdap_frame_error_code_t error_code,
    uint8_t *output,
    size_t output_capacity)
{
    if (!valid_error_code(error_code) || output == NULL ||
        output_capacity < AIRDAP_FRAME_ERROR_CODE_SIZE) {
        return false;
    }
    write_u16_be(output, (uint16_t) error_code);
    return true;
}

airdap_frame_decode_status_t airdap_frame_error_code_decode(
    const uint8_t *payload,
    size_t payload_size,
    airdap_frame_error_code_t *decoded_error_code,
    airdap_frame_error_code_t *parse_error_code)
{
    if (decoded_error_code == NULL || parse_error_code == NULL) {
        return AIRDAP_FRAME_DECODE_INVALID_FRAME;
    }
    *decoded_error_code = AIRDAP_FRAME_ERROR_NONE;
    *parse_error_code = AIRDAP_FRAME_ERROR_INTERNAL;
    if (payload == NULL && payload_size > 0U) {
        return AIRDAP_FRAME_DECODE_INVALID_FRAME;
    }
    if (payload_size < AIRDAP_FRAME_ERROR_CODE_SIZE) {
        *parse_error_code = AIRDAP_FRAME_ERROR_TRUNCATED;
        return AIRDAP_FRAME_DECODE_NEED_MORE_DATA;
    }
    if (payload_size > AIRDAP_FRAME_ERROR_CODE_SIZE) {
        *parse_error_code = AIRDAP_FRAME_ERROR_PAYLOAD_TOO_LARGE;
        return AIRDAP_FRAME_DECODE_INVALID_FRAME;
    }

    const airdap_frame_error_code_t error_code =
        (airdap_frame_error_code_t) read_u16_be(payload);
    if (!valid_error_code(error_code)) {
        *parse_error_code = AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE;
        return AIRDAP_FRAME_DECODE_INVALID_FRAME;
    }
    *decoded_error_code = error_code;
    *parse_error_code = AIRDAP_FRAME_ERROR_NONE;
    return AIRDAP_FRAME_DECODE_OK;
}

uint32_t airdap_frame_next_session_id(uint32_t current_session_id)
{
    const uint32_t next = current_session_id + 1U;
    return next == 0U ? AIRDAP_FRAME_INITIAL_SESSION_ID : next;
}

uint32_t airdap_frame_next_sequence(uint32_t current_sequence)
{
    const uint32_t next = current_sequence + 1U;
    return next == 0U ? AIRDAP_FRAME_INITIAL_SEQUENCE : next;
}

airdap_frame_error_code_t airdap_frame_sequence_validator_init(
    airdap_frame_sequence_validator_t *validator,
    uint32_t session_id)
{
    if (validator == NULL) {
        return AIRDAP_FRAME_ERROR_INTERNAL;
    }
    *validator = (airdap_frame_sequence_validator_t) {0};
    if (session_id == 0U) {
        return AIRDAP_FRAME_ERROR_SESSION_MISMATCH;
    }
    validator->session_id = session_id;
    validator->expected_sequence = AIRDAP_FRAME_INITIAL_SEQUENCE;
    return AIRDAP_FRAME_ERROR_NONE;
}

airdap_frame_error_code_t airdap_frame_validate_request_sequence(
    airdap_frame_sequence_validator_t *validator,
    const airdap_frame_header_t *request)
{
    if (validator == NULL || request == NULL) {
        return AIRDAP_FRAME_ERROR_INTERNAL;
    }
    const airdap_frame_error_code_t header_error = validate_header(request);
    if (header_error != AIRDAP_FRAME_ERROR_NONE) {
        return header_error;
    }
    if (!request_message_type(request->type)) {
        return AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE;
    }
    if (validator->session_id == 0U ||
        request->session_id != validator->session_id) {
        return AIRDAP_FRAME_ERROR_SESSION_MISMATCH;
    }
    if (request->sequence == validator->expected_sequence) {
        validator->last_sequence = request->sequence;
        validator->has_last_sequence = true;
        validator->expected_sequence =
            airdap_frame_next_sequence(request->sequence);
        return AIRDAP_FRAME_ERROR_NONE;
    }
    if (validator->has_last_sequence &&
        request->sequence == validator->last_sequence) {
        return AIRDAP_FRAME_ERROR_SEQUENCE_DUPLICATE;
    }
    if (!validator->has_last_sequence) {
        return AIRDAP_FRAME_ERROR_SEQUENCE_OUT_OF_ORDER;
    }

    const uint64_t sequence_space = UINT32_MAX;
    const uint64_t request_index = (uint64_t) request->sequence - 1U;
    const uint64_t expected_index =
        (uint64_t) validator->expected_sequence - 1U;
    const uint64_t forward_distance =
        (request_index + sequence_space - expected_index) % sequence_space;
    return forward_distance <= sequence_space / 2U
        ? AIRDAP_FRAME_ERROR_SEQUENCE_OUT_OF_ORDER
        : AIRDAP_FRAME_ERROR_SEQUENCE_STALE;
}

static uint8_t expected_response_type(uint8_t request_type)
{
    switch (request_type) {
    case AIRDAP_FRAME_TYPE_HELLO:
    case AIRDAP_FRAME_TYPE_AUTH:
    case AIRDAP_FRAME_TYPE_KEEPALIVE:
        return request_type;
    case AIRDAP_FRAME_TYPE_DAP_REQUEST:
        return AIRDAP_FRAME_TYPE_DAP_RESPONSE;
    case AIRDAP_FRAME_TYPE_CONTROL_REQUEST:
        return AIRDAP_FRAME_TYPE_CONTROL_RESPONSE;
    default:
        return 0U;
    }
}

airdap_frame_error_code_t airdap_frame_validate_response(
    const airdap_frame_header_t *request,
    const airdap_frame_header_t *response)
{
    const airdap_frame_error_code_t request_error =
        validate_header(request);
    if (request_error != AIRDAP_FRAME_ERROR_NONE) {
        return request_error;
    }
    const airdap_frame_error_code_t response_error =
        validate_header(response);
    if (response_error != AIRDAP_FRAME_ERROR_NONE) {
        return response_error;
    }
    const uint8_t expected_type = expected_response_type(request->type);
    if (expected_type == 0U) {
        return AIRDAP_FRAME_ERROR_RESPONSE_MISMATCH;
    }
    if (response->session_id != request->session_id) {
        return AIRDAP_FRAME_ERROR_SESSION_MISMATCH;
    }
    if (response->sequence != request->sequence ||
        (response->type != expected_type &&
         response->type != AIRDAP_FRAME_TYPE_ERROR)) {
        return AIRDAP_FRAME_ERROR_RESPONSE_MISMATCH;
    }
    return AIRDAP_FRAME_ERROR_NONE;
}
