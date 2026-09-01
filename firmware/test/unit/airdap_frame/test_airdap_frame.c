#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_frame.h"
#include "golden_vectors.h"

static airdap_frame_header_t request_header(
    uint32_t session_id,
    uint32_t sequence,
    airdap_frame_message_type_t type);

static void assert_decode_error(
    const uint8_t *frame,
    size_t frame_size,
    airdap_frame_decode_status_t expected_status,
    airdap_frame_error_code_t expected_error)
{
    airdap_frame_header_t header = {0};
    const uint8_t *payload = (const uint8_t *) 1;
    size_t decoded_size = 99U;
    airdap_frame_error_code_t error = AIRDAP_FRAME_ERROR_NONE;

    assert(airdap_frame_decode(
        frame,
        frame_size,
        &header,
        &payload,
        &decoded_size,
        &error) == expected_status);
    assert(error == expected_error);
    assert(payload == NULL);
    assert(decoded_size == 0U);
}

static void test_protocol_constants_are_stable(void)
{
    assert(AIRDAP_FRAME_MAGIC == UINT32_C(0x41444150));
    assert(AIRDAP_FRAME_HEADER_SIZE == 20U);
    assert(AIRDAP_FRAME_PROTOCOL_VERSION == 1U);
    assert(AIRDAP_FRAME_MAX_PAYLOAD_SIZE == 4096U);
    assert(AIRDAP_FRAME_DAP_REQUEST_MAX_PAYLOAD_SIZE == 508U);
    assert(AIRDAP_FRAME_INITIAL_SESSION_ID == 1U);
    assert(AIRDAP_FRAME_INITIAL_SEQUENCE == 1U);

    assert(AIRDAP_FRAME_TYPE_HELLO == 1);
    assert(AIRDAP_FRAME_TYPE_AUTH == 2);
    assert(AIRDAP_FRAME_TYPE_DAP_REQUEST == 3);
    assert(AIRDAP_FRAME_TYPE_DAP_RESPONSE == 4);
    assert(AIRDAP_FRAME_TYPE_CONTROL_REQUEST == 5);
    assert(AIRDAP_FRAME_TYPE_CONTROL_RESPONSE == 6);
    assert(AIRDAP_FRAME_TYPE_KEEPALIVE == 7);
    assert(AIRDAP_FRAME_TYPE_ERROR == 8);

    assert(AIRDAP_FRAME_ERROR_TRUNCATED == 0x0001);
    assert(AIRDAP_FRAME_ERROR_PAYLOAD_TOO_LARGE == 0x0002);
    assert(AIRDAP_FRAME_ERROR_UNSUPPORTED_VERSION == 0x0003);
    assert(AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE == 0x0004);
    assert(AIRDAP_FRAME_ERROR_INVALID_MAGIC == 0x0005);
    assert(AIRDAP_FRAME_ERROR_INVALID_FLAGS == 0x0006);
    assert(AIRDAP_FRAME_ERROR_INVALID_RESERVED == 0x0007);
    assert(AIRDAP_FRAME_ERROR_SESSION_MISMATCH == 0x0010);
    assert(AIRDAP_FRAME_ERROR_SEQUENCE_DUPLICATE == 0x0011);
    assert(AIRDAP_FRAME_ERROR_SEQUENCE_STALE == 0x0012);
    assert(AIRDAP_FRAME_ERROR_SEQUENCE_OUT_OF_ORDER == 0x0013);
    assert(AIRDAP_FRAME_ERROR_RESPONSE_MISMATCH == 0x0014);
    assert(AIRDAP_FRAME_ERROR_BUSY == 0x0020);
    assert(AIRDAP_FRAME_ERROR_UNAUTHENTICATED == 0x0021);
    assert(AIRDAP_FRAME_ERROR_TIMEOUT == 0x0022);
    assert(AIRDAP_FRAME_ERROR_INTERNAL == 0x00FF);
}

static void test_golden_frame_decodes_and_encodes(void)
{
    airdap_frame_header_t header = {0};
    const uint8_t *payload = NULL;
    size_t decoded_size = 0U;
    airdap_frame_error_code_t error = AIRDAP_FRAME_ERROR_INTERNAL;

    assert(airdap_frame_decode(
        AIRDAP_FRAME_GOLDEN_DAP_REQUEST,
        sizeof(AIRDAP_FRAME_GOLDEN_DAP_REQUEST),
        &header,
        &payload,
        &decoded_size,
        &error) == AIRDAP_FRAME_DECODE_OK);
    assert(error == AIRDAP_FRAME_ERROR_NONE);
    assert(header.magic == AIRDAP_FRAME_MAGIC);
    assert(header.version == AIRDAP_FRAME_PROTOCOL_VERSION);
    assert(header.type == AIRDAP_FRAME_TYPE_DAP_REQUEST);
    assert(header.flags == 0U);
    assert(header.session_id == UINT32_C(0x01020304));
    assert(header.sequence == UINT32_C(0xA1B2C3D4));
    assert(header.payload_length == 3U);
    assert(header.reserved == 0U);
    assert(decoded_size == sizeof(AIRDAP_FRAME_GOLDEN_DAP_REQUEST));
    assert(payload == AIRDAP_FRAME_GOLDEN_DAP_REQUEST + AIRDAP_FRAME_HEADER_SIZE);
    assert(memcmp(payload, "\x00\x7F\xFF", 3U) == 0);

    uint8_t encoded[sizeof(AIRDAP_FRAME_GOLDEN_DAP_REQUEST)] = {0};
    size_t encoded_size = 0U;
    assert(airdap_frame_encode(
        &header,
        payload,
        encoded,
        sizeof(encoded),
        &encoded_size) == AIRDAP_FRAME_ERROR_NONE);
    assert(encoded_size == sizeof(AIRDAP_FRAME_GOLDEN_DAP_REQUEST));
    assert(memcmp(
        encoded,
        AIRDAP_FRAME_GOLDEN_DAP_REQUEST,
        sizeof(encoded)) == 0);
}

static void test_truncated_header_and_payload_need_more_data(void)
{
    for (size_t length = 0U; length < AIRDAP_FRAME_HEADER_SIZE; ++length) {
        assert_decode_error(
            AIRDAP_FRAME_GOLDEN_DAP_REQUEST,
            length,
            AIRDAP_FRAME_DECODE_NEED_MORE_DATA,
            AIRDAP_FRAME_ERROR_TRUNCATED);
    }
    for (size_t length = AIRDAP_FRAME_HEADER_SIZE;
         length < sizeof(AIRDAP_FRAME_GOLDEN_DAP_REQUEST);
         ++length) {
        assert_decode_error(
            AIRDAP_FRAME_GOLDEN_DAP_REQUEST,
            length,
            AIRDAP_FRAME_DECODE_NEED_MORE_DATA,
            AIRDAP_FRAME_ERROR_TRUNCATED);
    }
}

static void test_invalid_golden_headers_are_rejected(void)
{
    assert_decode_error(
        AIRDAP_FRAME_GOLDEN_BAD_MAGIC,
        sizeof(AIRDAP_FRAME_GOLDEN_BAD_MAGIC),
        AIRDAP_FRAME_DECODE_INVALID_FRAME,
        AIRDAP_FRAME_ERROR_INVALID_MAGIC);
    assert_decode_error(
        AIRDAP_FRAME_GOLDEN_BAD_VERSION,
        sizeof(AIRDAP_FRAME_GOLDEN_BAD_VERSION),
        AIRDAP_FRAME_DECODE_INVALID_FRAME,
        AIRDAP_FRAME_ERROR_UNSUPPORTED_VERSION);
    assert_decode_error(
        AIRDAP_FRAME_GOLDEN_BAD_TYPE,
        sizeof(AIRDAP_FRAME_GOLDEN_BAD_TYPE),
        AIRDAP_FRAME_DECODE_INVALID_FRAME,
        AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE);
    assert_decode_error(
        AIRDAP_FRAME_GOLDEN_BAD_FLAGS,
        sizeof(AIRDAP_FRAME_GOLDEN_BAD_FLAGS),
        AIRDAP_FRAME_DECODE_INVALID_FRAME,
        AIRDAP_FRAME_ERROR_INVALID_FLAGS);
    assert_decode_error(
        AIRDAP_FRAME_GOLDEN_BAD_RESERVED,
        sizeof(AIRDAP_FRAME_GOLDEN_BAD_RESERVED),
        AIRDAP_FRAME_DECODE_INVALID_FRAME,
        AIRDAP_FRAME_ERROR_INVALID_RESERVED);
    assert_decode_error(
        AIRDAP_FRAME_GOLDEN_GLOBAL_OVERSIZE,
        sizeof(AIRDAP_FRAME_GOLDEN_GLOBAL_OVERSIZE),
        AIRDAP_FRAME_DECODE_INVALID_FRAME,
        AIRDAP_FRAME_ERROR_PAYLOAD_TOO_LARGE);
    assert_decode_error(
        AIRDAP_FRAME_GOLDEN_DAP_OVERSIZE,
        sizeof(AIRDAP_FRAME_GOLDEN_DAP_OVERSIZE),
        AIRDAP_FRAME_DECODE_INVALID_FRAME,
        AIRDAP_FRAME_ERROR_PAYLOAD_TOO_LARGE);
    assert_decode_error(
        AIRDAP_FRAME_GOLDEN_ZERO_SEQUENCE,
        sizeof(AIRDAP_FRAME_GOLDEN_ZERO_SEQUENCE),
        AIRDAP_FRAME_DECODE_INVALID_FRAME,
        AIRDAP_FRAME_ERROR_SEQUENCE_OUT_OF_ORDER);
    assert_decode_error(
        AIRDAP_FRAME_GOLDEN_ZERO_SESSION,
        sizeof(AIRDAP_FRAME_GOLDEN_ZERO_SESSION),
        AIRDAP_FRAME_DECODE_INVALID_FRAME,
        AIRDAP_FRAME_ERROR_SESSION_MISMATCH);
}

static void test_all_message_types_are_accepted(void)
{
    for (uint8_t type = AIRDAP_FRAME_TYPE_HELLO;
         type <= AIRDAP_FRAME_TYPE_ERROR;
         ++type) {
        uint8_t frame[AIRDAP_FRAME_HEADER_SIZE];
        memcpy(
            frame,
            AIRDAP_FRAME_GOLDEN_BAD_VERSION,
            AIRDAP_FRAME_HEADER_SIZE);
        frame[4] = AIRDAP_FRAME_PROTOCOL_VERSION;
        frame[5] = type;

        airdap_frame_header_t header = {0};
        const uint8_t *payload = NULL;
        size_t decoded_size = 0U;
        airdap_frame_error_code_t error = AIRDAP_FRAME_ERROR_INTERNAL;
        assert(airdap_frame_decode(
            frame,
            sizeof(frame),
            &header,
            &payload,
            &decoded_size,
            &error) == AIRDAP_FRAME_DECODE_OK);
        assert(error == AIRDAP_FRAME_ERROR_NONE);
        assert(header.type == type);
        assert(decoded_size == AIRDAP_FRAME_HEADER_SIZE);
        assert(payload == frame + AIRDAP_FRAME_HEADER_SIZE);
    }
}

static void test_encode_rejects_invalid_or_short_output(void)
{
    airdap_frame_header_t header = {
        .magic = AIRDAP_FRAME_MAGIC,
        .version = AIRDAP_FRAME_PROTOCOL_VERSION,
        .type = AIRDAP_FRAME_TYPE_DAP_REQUEST,
        .session_id = 1U,
        .sequence = 1U,
        .payload_length = 1U,
    };
    const uint8_t payload = 0xA5U;
    uint8_t output[AIRDAP_FRAME_HEADER_SIZE + 1U];
    size_t encoded_size = 0U;

    assert(airdap_frame_encode(
        &header,
        &payload,
        output,
        sizeof(output) - 1U,
        &encoded_size) == AIRDAP_FRAME_ERROR_TRUNCATED);
    assert(encoded_size == sizeof(output));

    header.flags = 1U;
    assert(airdap_frame_encode(
        &header,
        &payload,
        output,
        sizeof(output),
        &encoded_size) == AIRDAP_FRAME_ERROR_INVALID_FLAGS);
}

static void test_payload_limits_accept_exact_boundaries(void)
{
    static uint8_t payload[AIRDAP_FRAME_MAX_PAYLOAD_SIZE];
    static uint8_t output[
        AIRDAP_FRAME_HEADER_SIZE + AIRDAP_FRAME_MAX_PAYLOAD_SIZE];
    size_t encoded_size = 0U;
    airdap_frame_header_t header = request_header(
        1U, 1U, AIRDAP_FRAME_TYPE_DAP_REQUEST);
    header.payload_length = AIRDAP_FRAME_DAP_REQUEST_MAX_PAYLOAD_SIZE;
    assert(airdap_frame_encode(
        &header,
        payload,
        output,
        sizeof(output),
        &encoded_size) == AIRDAP_FRAME_ERROR_NONE);
    assert(encoded_size == AIRDAP_FRAME_HEADER_SIZE +
        AIRDAP_FRAME_DAP_REQUEST_MAX_PAYLOAD_SIZE);

    header.type = AIRDAP_FRAME_TYPE_CONTROL_REQUEST;
    header.payload_length = AIRDAP_FRAME_MAX_PAYLOAD_SIZE;
    assert(airdap_frame_encode(
        &header,
        payload,
        output,
        sizeof(output),
        &encoded_size) == AIRDAP_FRAME_ERROR_NONE);
    assert(encoded_size == sizeof(output));

    airdap_frame_header_t decoded_header = {0};
    const uint8_t *decoded_payload = NULL;
    size_t decoded_size = 0U;
    airdap_frame_error_code_t error = AIRDAP_FRAME_ERROR_INTERNAL;
    assert(airdap_frame_decode(
        output,
        encoded_size,
        &decoded_header,
        &decoded_payload,
        &decoded_size,
        &error) == AIRDAP_FRAME_DECODE_OK);
    assert(decoded_header.payload_length == AIRDAP_FRAME_MAX_PAYLOAD_SIZE);
    assert(decoded_payload == output + AIRDAP_FRAME_HEADER_SIZE);
    assert(decoded_size == sizeof(output));
}

static void test_error_codes_have_network_order_payloads(void)
{
    uint8_t encoded[2] = {0};
    assert(airdap_frame_error_code_encode(
        AIRDAP_FRAME_ERROR_BUSY,
        encoded,
        sizeof(encoded)));
    assert(memcmp(
        encoded,
        AIRDAP_FRAME_GOLDEN_BUSY_ERROR,
        sizeof(encoded)) == 0);
    airdap_frame_error_code_t decoded = AIRDAP_FRAME_ERROR_NONE;
    airdap_frame_error_code_t parse_error = AIRDAP_FRAME_ERROR_INTERNAL;
    assert(airdap_frame_error_code_decode(
        AIRDAP_FRAME_GOLDEN_BUSY_ERROR,
        sizeof(AIRDAP_FRAME_GOLDEN_BUSY_ERROR),
        &decoded,
        &parse_error) == AIRDAP_FRAME_DECODE_OK);
    assert(decoded == AIRDAP_FRAME_ERROR_BUSY);
    assert(parse_error == AIRDAP_FRAME_ERROR_NONE);

    assert(airdap_frame_error_code_decode(
        AIRDAP_FRAME_GOLDEN_TRUNCATED_ERROR,
        sizeof(AIRDAP_FRAME_GOLDEN_TRUNCATED_ERROR),
        &decoded,
        &parse_error) == AIRDAP_FRAME_DECODE_OK);
    assert(decoded == AIRDAP_FRAME_ERROR_TRUNCATED);
    assert(parse_error == AIRDAP_FRAME_ERROR_NONE);
    assert(airdap_frame_error_code_decode(
        AIRDAP_FRAME_GOLDEN_INTERNAL_ERROR,
        sizeof(AIRDAP_FRAME_GOLDEN_INTERNAL_ERROR),
        &decoded,
        &parse_error) == AIRDAP_FRAME_DECODE_OK);
    assert(decoded == AIRDAP_FRAME_ERROR_INTERNAL);
    assert(parse_error == AIRDAP_FRAME_ERROR_NONE);

    assert(airdap_frame_error_code_decode(
        encoded,
        1U,
        &decoded,
        &parse_error) == AIRDAP_FRAME_DECODE_NEED_MORE_DATA);
    assert(decoded == AIRDAP_FRAME_ERROR_NONE);
    assert(parse_error == AIRDAP_FRAME_ERROR_TRUNCATED);
    uint8_t oversized_error[] = {0x00, 0x20, 0x00};
    assert(airdap_frame_error_code_decode(
        oversized_error,
        sizeof(oversized_error),
        &decoded,
        &parse_error) == AIRDAP_FRAME_DECODE_INVALID_FRAME);
    assert(parse_error == AIRDAP_FRAME_ERROR_PAYLOAD_TOO_LARGE);
    assert(airdap_frame_error_code_decode(
        AIRDAP_FRAME_GOLDEN_UNKNOWN_ERROR,
        sizeof(AIRDAP_FRAME_GOLDEN_UNKNOWN_ERROR),
        &decoded,
        &parse_error) == AIRDAP_FRAME_DECODE_INVALID_FRAME);
    assert(parse_error == AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE);
}

static airdap_frame_header_t request_header(
    uint32_t session_id,
    uint32_t sequence,
    airdap_frame_message_type_t type)
{
    return (airdap_frame_header_t) {
        .magic = AIRDAP_FRAME_MAGIC,
        .version = AIRDAP_FRAME_PROTOCOL_VERSION,
        .type = (uint8_t) type,
        .session_id = session_id,
        .sequence = sequence,
    };
}

static void test_request_sequence_validation(void)
{
    airdap_frame_sequence_validator_t validator;
    assert(airdap_frame_sequence_validator_init(&validator, 0U) ==
        AIRDAP_FRAME_ERROR_SESSION_MISMATCH);
    assert(airdap_frame_sequence_validator_init(&validator, 7U) ==
        AIRDAP_FRAME_ERROR_NONE);

    airdap_frame_header_t request = request_header(
        7U, 1U, AIRDAP_FRAME_TYPE_DAP_REQUEST);
    assert(airdap_frame_validate_request_sequence(&validator, &request) ==
        AIRDAP_FRAME_ERROR_NONE);
    assert(validator.expected_sequence == 2U);
    assert(airdap_frame_validate_request_sequence(&validator, &request) ==
        AIRDAP_FRAME_ERROR_SEQUENCE_DUPLICATE);

    request.sequence = 3U;
    assert(airdap_frame_validate_request_sequence(&validator, &request) ==
        AIRDAP_FRAME_ERROR_SEQUENCE_OUT_OF_ORDER);
    request.sequence = 0U;
    assert(airdap_frame_validate_request_sequence(&validator, &request) ==
        AIRDAP_FRAME_ERROR_SEQUENCE_OUT_OF_ORDER);
    request.sequence = 2U;
    assert(airdap_frame_validate_request_sequence(&validator, &request) ==
        AIRDAP_FRAME_ERROR_NONE);
    request.sequence = 1U;
    assert(airdap_frame_validate_request_sequence(&validator, &request) ==
        AIRDAP_FRAME_ERROR_SEQUENCE_STALE);

    request.session_id = 8U;
    request.sequence = 3U;
    assert(airdap_frame_validate_request_sequence(&validator, &request) ==
        AIRDAP_FRAME_ERROR_SESSION_MISMATCH);
    assert(validator.expected_sequence == 3U);
}

static void test_golden_duplicate_and_cross_session_are_rejected(void)
{
    airdap_frame_header_t request = {0};
    airdap_frame_header_t response = {0};
    const uint8_t *payload = NULL;
    size_t frame_size = 0U;
    airdap_frame_error_code_t error = AIRDAP_FRAME_ERROR_INTERNAL;

    assert(airdap_frame_decode(
        AIRDAP_FRAME_GOLDEN_SEQUENCE_ONE_REQUEST,
        sizeof(AIRDAP_FRAME_GOLDEN_SEQUENCE_ONE_REQUEST),
        &request,
        &payload,
        &frame_size,
        &error) == AIRDAP_FRAME_DECODE_OK);
    assert(airdap_frame_decode(
        AIRDAP_FRAME_GOLDEN_CROSS_SESSION_RESPONSE,
        sizeof(AIRDAP_FRAME_GOLDEN_CROSS_SESSION_RESPONSE),
        &response,
        &payload,
        &frame_size,
        &error) == AIRDAP_FRAME_DECODE_OK);

    airdap_frame_sequence_validator_t validator;
    assert(airdap_frame_sequence_validator_init(&validator, 1U) ==
        AIRDAP_FRAME_ERROR_NONE);
    assert(airdap_frame_validate_request_sequence(&validator, &request) ==
        AIRDAP_FRAME_ERROR_NONE);
    assert(airdap_frame_validate_request_sequence(&validator, &request) ==
        AIRDAP_FRAME_ERROR_SEQUENCE_DUPLICATE);
    assert(airdap_frame_validate_response(&request, &response) ==
        AIRDAP_FRAME_ERROR_SESSION_MISMATCH);
}

static void test_sequence_and_session_wrap_skip_zero(void)
{
    assert(airdap_frame_next_sequence(UINT32_MAX) ==
        AIRDAP_FRAME_INITIAL_SEQUENCE);
    assert(airdap_frame_next_session_id(UINT32_MAX) ==
        AIRDAP_FRAME_INITIAL_SESSION_ID);

    airdap_frame_sequence_validator_t validator = {
        .session_id = 9U,
        .expected_sequence = UINT32_MAX,
        .last_sequence = UINT32_MAX - 1U,
        .has_last_sequence = true,
    };
    airdap_frame_header_t request = request_header(
        9U, UINT32_MAX, AIRDAP_FRAME_TYPE_DAP_REQUEST);
    assert(airdap_frame_validate_request_sequence(&validator, &request) ==
        AIRDAP_FRAME_ERROR_NONE);
    assert(validator.expected_sequence == AIRDAP_FRAME_INITIAL_SEQUENCE);
    request.sequence = UINT32_MAX - 1U;
    assert(airdap_frame_validate_request_sequence(&validator, &request) ==
        AIRDAP_FRAME_ERROR_SEQUENCE_STALE);
    request.sequence = AIRDAP_FRAME_INITIAL_SEQUENCE + 1U;
    assert(airdap_frame_validate_request_sequence(&validator, &request) ==
        AIRDAP_FRAME_ERROR_SEQUENCE_OUT_OF_ORDER);
    request.sequence = UINT32_MAX;
    assert(airdap_frame_validate_request_sequence(&validator, &request) ==
        AIRDAP_FRAME_ERROR_SEQUENCE_DUPLICATE);
    request.sequence = AIRDAP_FRAME_INITIAL_SEQUENCE;
    assert(airdap_frame_validate_request_sequence(&validator, &request) ==
        AIRDAP_FRAME_ERROR_NONE);
}

static void test_response_must_match_request_session_sequence_and_type(void)
{
    const airdap_frame_header_t request = request_header(
        11U, 42U, AIRDAP_FRAME_TYPE_DAP_REQUEST);
    airdap_frame_header_t response = request_header(
        11U, 42U, AIRDAP_FRAME_TYPE_DAP_RESPONSE);

    assert(airdap_frame_validate_response(&request, &response) ==
        AIRDAP_FRAME_ERROR_NONE);
    response.session_id = 12U;
    assert(airdap_frame_validate_response(&request, &response) ==
        AIRDAP_FRAME_ERROR_SESSION_MISMATCH);
    response.session_id = 11U;
    response.sequence = 43U;
    assert(airdap_frame_validate_response(&request, &response) ==
        AIRDAP_FRAME_ERROR_RESPONSE_MISMATCH);
    response.sequence = 42U;
    response.type = AIRDAP_FRAME_TYPE_CONTROL_RESPONSE;
    assert(airdap_frame_validate_response(&request, &response) ==
        AIRDAP_FRAME_ERROR_RESPONSE_MISMATCH);
    response.type = AIRDAP_FRAME_TYPE_ERROR;
    assert(airdap_frame_validate_response(&request, &response) ==
        AIRDAP_FRAME_ERROR_NONE);

    const airdap_frame_header_t control_request = request_header(
        11U, 43U, AIRDAP_FRAME_TYPE_CONTROL_REQUEST);
    response.type = AIRDAP_FRAME_TYPE_CONTROL_RESPONSE;
    response.sequence = 43U;
    assert(airdap_frame_validate_response(&control_request, &response) ==
        AIRDAP_FRAME_ERROR_NONE);

    const airdap_frame_message_type_t same_type_responses[] = {
        AIRDAP_FRAME_TYPE_HELLO,
        AIRDAP_FRAME_TYPE_AUTH,
        AIRDAP_FRAME_TYPE_KEEPALIVE,
    };
    for (size_t index = 0U;
         index < sizeof(same_type_responses) /
            sizeof(same_type_responses[0]);
         ++index) {
        const airdap_frame_header_t same_type_request = request_header(
            11U, (uint32_t) (44U + index), same_type_responses[index]);
        const airdap_frame_header_t same_type_response = request_header(
            11U, (uint32_t) (44U + index), same_type_responses[index]);
        assert(airdap_frame_validate_response(
            &same_type_request,
            &same_type_response) == AIRDAP_FRAME_ERROR_NONE);
    }
}

static void test_null_buffer_contract_is_unambiguous(void)
{
    airdap_frame_header_t header = {0};
    const uint8_t *payload = NULL;
    size_t frame_size = 0U;
    airdap_frame_error_code_t error = AIRDAP_FRAME_ERROR_NONE;
    assert(airdap_frame_decode(
        NULL,
        0U,
        &header,
        &payload,
        &frame_size,
        &error) == AIRDAP_FRAME_DECODE_NEED_MORE_DATA);
    assert(error == AIRDAP_FRAME_ERROR_TRUNCATED);
    assert(airdap_frame_decode(
        NULL,
        1U,
        &header,
        &payload,
        &frame_size,
        &error) == AIRDAP_FRAME_DECODE_INVALID_FRAME);
    assert(error == AIRDAP_FRAME_ERROR_INTERNAL);

    header = request_header(1U, 1U, AIRDAP_FRAME_TYPE_HELLO);
    size_t encoded_size = 0U;
    assert(airdap_frame_encode(
        &header,
        NULL,
        NULL,
        0U,
        &encoded_size) == AIRDAP_FRAME_ERROR_TRUNCATED);
    assert(encoded_size == AIRDAP_FRAME_HEADER_SIZE);
}

int main(void)
{
    test_protocol_constants_are_stable();
    test_golden_frame_decodes_and_encodes();
    test_truncated_header_and_payload_need_more_data();
    test_invalid_golden_headers_are_rejected();
    test_all_message_types_are_accepted();
    test_encode_rejects_invalid_or_short_output();
    test_payload_limits_accept_exact_boundaries();
    test_error_codes_have_network_order_payloads();
    test_request_sequence_validation();
    test_golden_duplicate_and_cross_session_are_rejected();
    test_sequence_and_session_wrap_skip_zero();
    test_response_must_match_request_session_sequence_and_type();
    test_null_buffer_contract_is_unambiguous();
    puts("AirDAP frame protocol tests passed");
    return 0;
}
