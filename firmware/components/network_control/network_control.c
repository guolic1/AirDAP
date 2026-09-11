#include "airdap_board.h"
#include "airdap_control.h"
#include "airdap_mode_state.h"
#include "airdap_network_control.h"

airdap_frame_error_code_t airdap_network_control_dispatch(
    airdap_network_auth_connection_t *connection,
    uint32_t session_id,
    const uint8_t *payload,
    size_t payload_size,
    uint8_t response[2],
    size_t *response_size)
{
    if (response_size == NULL) {
        return AIRDAP_FRAME_ERROR_INTERNAL;
    }
    *response_size = 0U;
    if (response == NULL) {
        return AIRDAP_FRAME_ERROR_INTERNAL;
    }
    if (connection == NULL || session_id == 0U ||
        airdap_network_auth_session_validate(connection, session_id) !=
            AIRDAP_NETWORK_AUTH_OK) {
        return AIRDAP_FRAME_ERROR_UNAUTHENTICATED;
    }
    if (payload_size == 0U) {
        return AIRDAP_FRAME_ERROR_TRUNCATED;
    }
    if (payload == NULL) {
        return AIRDAP_FRAME_ERROR_INVALID_ARGUMENT;
    }
    const uint8_t opcode = payload[0];
    size_t required_size;
    switch (opcode) {
    case AIRDAP_CONTROL_RESET_SET:
    case AIRDAP_CONTROL_POWER_SET:
        required_size = 2U;
        break;
    case AIRDAP_CONTROL_POWER_GET:
        required_size = 1U;
        break;
    default:
        return AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE;
    }
    if (payload_size < required_size) {
        return AIRDAP_FRAME_ERROR_TRUNCATED;
    }
    if (payload_size != required_size ||
        (required_size == 2U && payload[1] > 1U)) {
        return AIRDAP_FRAME_ERROR_INVALID_ARGUMENT;
    }
    airdap_dap_ownership_operation_t operation = {0};
    const airdap_mode_dap_result_t admission =
        airdap_mode_state_control_operation_begin(true, &operation);
    if (admission != AIRDAP_MODE_DAP_ALLOWED) {
        return admission == AIRDAP_MODE_DAP_BUSY ||
            admission == AIRDAP_MODE_DAP_OFFLINE
            ? AIRDAP_FRAME_ERROR_BUSY : AIRDAP_FRAME_ERROR_INTERNAL;
    }
    /* Changing power permission while DAP retains driven pins is unsafe.
     * Require DAP_Disconnect first; reservation keeps this check stable. */
    if (opcode == AIRDAP_CONTROL_POWER_SET &&
        operation.owner != AIRDAP_DAP_OWNER_NONE) {
        airdap_dap_ownership_operation_end(&operation);
        return AIRDAP_FRAME_ERROR_BUSY;
    }
    airdap_frame_error_code_t result = AIRDAP_FRAME_ERROR_UNAUTHENTICATED;
    if (airdap_network_auth_session_validate(connection, session_id) ==
        AIRDAP_NETWORK_AUTH_OK) {
        bool value = required_size == 2U && payload[1] != 0U;
        esp_err_t error;
        switch (opcode) {
        case AIRDAP_CONTROL_RESET_SET:
            error = airdap_target_reset_set_asserted(value);
            break;
        case AIRDAP_CONTROL_POWER_SET:
            error = airdap_target_power_set_allowed(value);
            break;
        default:
            error = airdap_target_power_get_active(&value);
            break;
        }
        result = error == ESP_OK
            ? AIRDAP_FRAME_ERROR_NONE : AIRDAP_FRAME_ERROR_INTERNAL;
        if (error == ESP_OK) {
            response[0] = opcode;
            response[1] = value ? 1U : 0U;
            *response_size = 2U;
        }
    }
    airdap_dap_ownership_operation_end(&operation);
    return result;
}
