#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AIRDAP_FRAME_MAGIC = UINT32_C(0x41444150),
    AIRDAP_FRAME_HEADER_SIZE = 20,
    AIRDAP_FRAME_PROTOCOL_VERSION = 1,
    AIRDAP_FRAME_MAX_PAYLOAD_SIZE = 4096,
    AIRDAP_FRAME_DAP_REQUEST_MAX_PAYLOAD_SIZE = 508,
    AIRDAP_FRAME_INITIAL_SESSION_ID = 1,
    AIRDAP_FRAME_INITIAL_SEQUENCE = 1,
    AIRDAP_FRAME_ERROR_CODE_SIZE = 2,
};

typedef enum {
    AIRDAP_FRAME_TYPE_HELLO = 1,
    AIRDAP_FRAME_TYPE_AUTH = 2,
    AIRDAP_FRAME_TYPE_DAP_REQUEST = 3,
    AIRDAP_FRAME_TYPE_DAP_RESPONSE = 4,
    AIRDAP_FRAME_TYPE_CONTROL_REQUEST = 5,
    AIRDAP_FRAME_TYPE_CONTROL_RESPONSE = 6,
    AIRDAP_FRAME_TYPE_KEEPALIVE = 7,
    AIRDAP_FRAME_TYPE_ERROR = 8,
} airdap_frame_message_type_t;

/* These values are part of the wire protocol. ERROR payloads encode one value
 * as an unsigned 16-bit integer in network byte order. */
typedef enum {
    AIRDAP_FRAME_ERROR_NONE = 0x0000,
    AIRDAP_FRAME_ERROR_TRUNCATED = 0x0001,
    AIRDAP_FRAME_ERROR_PAYLOAD_TOO_LARGE = 0x0002,
    AIRDAP_FRAME_ERROR_UNSUPPORTED_VERSION = 0x0003,
    AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE = 0x0004,
    AIRDAP_FRAME_ERROR_INVALID_MAGIC = 0x0005,
    AIRDAP_FRAME_ERROR_INVALID_FLAGS = 0x0006,
    AIRDAP_FRAME_ERROR_INVALID_RESERVED = 0x0007,

    AIRDAP_FRAME_ERROR_SESSION_MISMATCH = 0x0010,
    AIRDAP_FRAME_ERROR_SEQUENCE_DUPLICATE = 0x0011,
    AIRDAP_FRAME_ERROR_SEQUENCE_STALE = 0x0012,
    AIRDAP_FRAME_ERROR_SEQUENCE_OUT_OF_ORDER = 0x0013,
    AIRDAP_FRAME_ERROR_RESPONSE_MISMATCH = 0x0014,

    AIRDAP_FRAME_ERROR_BUSY = 0x0020,
    AIRDAP_FRAME_ERROR_UNAUTHENTICATED = 0x0021,
    AIRDAP_FRAME_ERROR_TIMEOUT = 0x0022,
    AIRDAP_FRAME_ERROR_INTERNAL = 0x00FF,
} airdap_frame_error_code_t;

typedef enum {
    AIRDAP_FRAME_DECODE_OK = 0,
    AIRDAP_FRAME_DECODE_NEED_MORE_DATA,
    AIRDAP_FRAME_DECODE_INVALID_FRAME,
} airdap_frame_decode_status_t;

/* This is the decoded representation, not an overlay on wire bytes. Code must
 * use airdap_frame_encode()/airdap_frame_decode() for serialization. */
typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t flags;
    uint32_t session_id;
    uint32_t sequence;
    uint16_t payload_length;
    uint16_t reserved;
} airdap_frame_header_t;

typedef struct {
    uint32_t session_id;
    uint32_t expected_sequence;
    uint32_t last_sequence;
    bool has_last_sequence;
} airdap_frame_sequence_validator_t;

/* On success, payload points into input and frame_size identifies the first
 * complete frame, allowing callers to retain any following bytes. Incomplete
 * header or payload data returns NEED_MORE_DATA with ERROR_TRUNCATED. Input
 * may be NULL only when input_size is zero; all output pointers are required. */
airdap_frame_decode_status_t airdap_frame_decode(
    const uint8_t *input,
    size_t input_size,
    airdap_frame_header_t *header,
    const uint8_t **payload,
    size_t *frame_size,
    airdap_frame_error_code_t *error_code);

/* encoded_size receives the required frame size even when output is short.
 * Output may be NULL with zero capacity to query that size. */
airdap_frame_error_code_t airdap_frame_encode(
    const airdap_frame_header_t *header,
    const uint8_t *payload,
    uint8_t *output,
    size_t output_capacity,
    size_t *encoded_size);

bool airdap_frame_error_code_encode(
    airdap_frame_error_code_t error_code,
    uint8_t *output,
    size_t output_capacity);

/* V1 ERROR payloads contain exactly one 16-bit code. The parse status/error
 * are separate from decoded_error_code so a peer's TRUNCATED or INTERNAL code
 * cannot be confused with a malformed local payload. */
airdap_frame_decode_status_t airdap_frame_error_code_decode(
    const uint8_t *payload,
    size_t payload_size,
    airdap_frame_error_code_t *decoded_error_code,
    airdap_frame_error_code_t *parse_error_code);

uint32_t airdap_frame_next_session_id(uint32_t current_session_id);
uint32_t airdap_frame_next_sequence(uint32_t current_sequence);

airdap_frame_error_code_t airdap_frame_sequence_validator_init(
    airdap_frame_sequence_validator_t *validator,
    uint32_t session_id);

/* Accepts only the next sequence for the validator's session. Replaying the
 * immediately previous request is duplicate; older values are stale; values
 * ahead of the expected sequence are out of order. */
airdap_frame_error_code_t airdap_frame_validate_request_sequence(
    airdap_frame_sequence_validator_t *validator,
    const airdap_frame_header_t *request);

/* A response must use the request's session and sequence. DAP and CONTROL
 * requests require their matching response type; HELLO, AUTH and KEEPALIVE
 * use the same type in both directions. ERROR may answer any request type. */
airdap_frame_error_code_t airdap_frame_validate_response(
    const airdap_frame_header_t *request,
    const airdap_frame_header_t *response);

#ifdef __cplusplus
}
#endif
