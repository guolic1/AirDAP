#include <string.h>

#include "airdap_network_ota.h"
#include "airdap_ota.h"
#include "esp_log.h"

static bool authorize(void *connection, uint32_t session_id)
{
    return connection != NULL && session_id != 0U &&
        airdap_network_auth_session_validate(connection, session_id) == AIRDAP_NETWORK_AUTH_OK;
}

static uint32_t read_u32(const uint8_t *data)
{
    return ((uint32_t) data[0] << 24) | ((uint32_t) data[1] << 16) |
        ((uint32_t) data[2] << 8) | data[3];
}

static void write_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t) (value >> 24);
    data[1] = (uint8_t) (value >> 16);
    data[2] = (uint8_t) (value >> 8);
    data[3] = (uint8_t) value;
}

airdap_frame_error_code_t airdap_network_ota_dispatch(
    airdap_network_auth_connection_t *connection, uint32_t session_id,
    const uint8_t *request, size_t request_size,
    uint8_t *response, size_t response_capacity, size_t *response_size)
{
    if (response_size == NULL) return AIRDAP_FRAME_ERROR_INTERNAL;
    *response_size = 0U;
    if (!authorize(connection, session_id)) return AIRDAP_FRAME_ERROR_UNAUTHENTICATED;
    if (response == NULL || response_capacity < AIRDAP_NETWORK_OTA_MAX_RESPONSE)
        return AIRDAP_FRAME_ERROR_INTERNAL;
    if (request_size == 0U) return AIRDAP_FRAME_ERROR_TRUNCATED;
    if (request == NULL) return AIRDAP_FRAME_ERROR_INVALID_ARGUMENT;
    const uint8_t opcode = request[0];
    if (opcode < AIRDAP_CONTROL_OTA_QUERY || opcode > AIRDAP_CONTROL_OTA_REBOOT)
        return AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE;
    const size_t minimum = opcode == AIRDAP_CONTROL_OTA_WRITE ? 6U :
        opcode == AIRDAP_CONTROL_OTA_BEGIN ? 5U : 1U;
    if (request_size < minimum) return AIRDAP_FRAME_ERROR_TRUNCATED;
    if ((opcode != AIRDAP_CONTROL_OTA_WRITE && request_size != minimum) ||
        request_size > 5U + AIRDAP_NETWORK_OTA_MAX_DATA)
        return AIRDAP_FRAME_ERROR_INVALID_ARGUMENT;
    const airdap_ota_client_t client = {connection, session_id, authorize};
    airdap_ota_status_t status;
    size_t size = 2U;
    uint32_t next_offset = 0U;
    switch (opcode) {
    case AIRDAP_CONTROL_OTA_QUERY: {
        airdap_ota_info_t info;
        status = airdap_ota_get_info(&info);
        if (status != AIRDAP_OTA_STATUS_OK) break;
        response[2] = info.protocol_version;
        response[3] = info.flags;
        write_u32(response + 4, info.max_image_size);
        write_u32(response + 8, info.running_address);
        write_u32(response + 12, info.update_address);
        response[16] = info.running_valid ? 1U : 0U;
        const size_t version_size = strlen(info.running_version);
        memcpy(response + 17, info.running_version, version_size);
        size = 17U + version_size;
        break;
    }
    case AIRDAP_CONTROL_OTA_BEGIN:
        status = airdap_ota_begin_for_client(&client, read_u32(request + 1));
        break;
    case AIRDAP_CONTROL_OTA_WRITE:
        status = airdap_ota_write_for_client(&client, read_u32(request + 1),
            request + 5, request_size - 5, &next_offset);
        if (status == AIRDAP_OTA_STATUS_OK) {
            write_u32(response + 2, next_offset);
            size = 6U;
        }
        break;
    case AIRDAP_CONTROL_OTA_COMMIT:
        status = airdap_ota_commit_for_client(&client);
        break;
    case AIRDAP_CONTROL_OTA_ABORT:
        status = airdap_ota_abort_for_client(&client);
        break;
    default:
        status = airdap_ota_prepare_reboot(&client);
        break;
    }
    if (status == AIRDAP_OTA_STATUS_UNAUTHENTICATED) return AIRDAP_FRAME_ERROR_UNAUTHENTICATED;
    if (status == AIRDAP_OTA_STATUS_BUSY) return AIRDAP_FRAME_ERROR_BUSY;
    response[0] = opcode;
    response[1] = (uint8_t) status;
    *response_size = status == AIRDAP_OTA_STATUS_OK ? size : 2U;
    return AIRDAP_FRAME_ERROR_NONE;
}

void airdap_network_ota_disconnect(
    airdap_network_auth_connection_t *connection, uint32_t session_id)
{
    if (connection == NULL || session_id == 0U) return;
    const airdap_ota_client_t client = {connection, session_id, authorize};
    const airdap_ota_status_t result = airdap_ota_disconnect_client(&client);
    if (result != AIRDAP_OTA_STATUS_OK) {
        ESP_LOGW("airdap_net_ota", "Disconnect abort failed (%u); authenticated ABORT may retry",
            (unsigned int) result);
    }
}

void airdap_network_ota_reboot(
    airdap_network_auth_connection_t *connection, uint32_t session_id)
{
    const airdap_ota_client_t client = {connection, session_id, authorize};
    (void) airdap_ota_reboot_for_client(&client);
}
