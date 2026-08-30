#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "airdap_dap_ota.h"
#include "airdap_ota.h"

enum {
    OTA_QUERY = 0x80,
    OTA_BEGIN = 0x81,
    OTA_WRITE = 0x82,
    OTA_COMMIT = 0x83,
    OTA_ABORT = 0x84,
    OTA_REBOOT = 0x85,
    OTA_WRITE_HEADER_SIZE = 7,
    OTA_MAX_WRITE_SIZE = 496,
};

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t) data[0] | ((uint16_t) data[1] << 8U);
}

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t) data[0] |
        ((uint32_t) data[1] << 8U) |
        ((uint32_t) data[2] << 16U) |
        ((uint32_t) data[3] << 24U);
}

static void write_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t) value;
    data[1] = (uint8_t) (value >> 8U);
    data[2] = (uint8_t) (value >> 16U);
    data[3] = (uint8_t) (value >> 24U);
}

static size_t status_response(
    uint8_t command,
    airdap_ota_status_t status,
    uint8_t *response,
    size_t response_capacity)
{
    if (response_capacity < 2U) {
        return 0U;
    }
    response[0] = command;
    response[1] = (uint8_t) status;
    return 2U;
}

static size_t process_query(uint8_t *response, size_t response_capacity)
{
    airdap_ota_info_t info;
    const airdap_ota_status_t status = airdap_ota_get_info(&info);
    if (status != AIRDAP_OTA_STATUS_OK) {
        return status_response(OTA_QUERY, status, response, response_capacity);
    }

    size_t version_length = 0U;
    while (version_length < AIRDAP_OTA_VERSION_CAPACITY &&
           info.running_version[version_length] != '\0') {
        ++version_length;
    }
    const size_t response_length = 9U + version_length;
    if (response_capacity < response_length) {
        return 0U;
    }

    response[0] = OTA_QUERY;
    response[1] = AIRDAP_OTA_STATUS_OK;
    response[2] = info.protocol_version;
    response[3] = info.flags;
    write_u32(response + 4U, info.max_image_size);
    response[8] = (uint8_t) version_length;
    memcpy(response + 9U, info.running_version, version_length);
    return response_length;
}

static size_t process_write(
    const uint8_t *request,
    size_t request_length,
    uint8_t *response,
    size_t response_capacity)
{
    if (request_length < OTA_WRITE_HEADER_SIZE) {
        return status_response(
            OTA_WRITE,
            AIRDAP_OTA_STATUS_INVALID_ARGUMENT,
            response,
            response_capacity);
    }

    const size_t data_length = read_u16(request + 5U);
    if (data_length > OTA_MAX_WRITE_SIZE ||
        request_length != OTA_WRITE_HEADER_SIZE + data_length) {
        return status_response(
            OTA_WRITE,
            AIRDAP_OTA_STATUS_INVALID_ARGUMENT,
            response,
            response_capacity);
    }

    uint32_t next_offset = 0U;
    const airdap_ota_status_t status = airdap_ota_write(
        read_u32(request + 1U),
        request + OTA_WRITE_HEADER_SIZE,
        data_length,
        &next_offset);
    const size_t response_length = status_response(
        OTA_WRITE, status, response, response_capacity);
    if (status != AIRDAP_OTA_STATUS_OK || response_length == 0U) {
        return response_length;
    }
    if (response_capacity < 6U) {
        return 0U;
    }
    write_u32(response + 2U, next_offset);
    return 6U;
}

size_t airdap_dap_ota_process(
    bool debug_connected,
    const uint8_t *request,
    size_t request_length,
    uint8_t *response,
    size_t response_capacity)
{
    if (request == NULL || request_length == 0U || response == NULL) {
        return 0U;
    }

    const uint8_t command = request[0];
    if (command < OTA_QUERY || command > OTA_REBOOT) {
        return 0U;
    }
    if (command != OTA_QUERY && debug_connected) {
        return status_response(
            command,
            AIRDAP_OTA_STATUS_INVALID_STATE,
            response,
            response_capacity);
    }

    switch (command) {
    case OTA_QUERY:
        if (request_length != 1U) {
            return status_response(
                command,
                AIRDAP_OTA_STATUS_INVALID_ARGUMENT,
                response,
                response_capacity);
        }
        return process_query(response, response_capacity);

    case OTA_BEGIN:
        if (request_length != 5U) {
            return status_response(
                command,
                AIRDAP_OTA_STATUS_INVALID_ARGUMENT,
                response,
                response_capacity);
        }
        return status_response(
            command,
            airdap_ota_begin(read_u32(request + 1U)),
            response,
            response_capacity);

    case OTA_WRITE:
        return process_write(
            request, request_length, response, response_capacity);

    case OTA_COMMIT:
        if (request_length != 1U) {
            return status_response(
                command,
                AIRDAP_OTA_STATUS_INVALID_ARGUMENT,
                response,
                response_capacity);
        }
        return status_response(
            command, airdap_ota_commit(), response, response_capacity);

    case OTA_ABORT:
        if (request_length != 1U) {
            return status_response(
                command,
                AIRDAP_OTA_STATUS_INVALID_ARGUMENT,
                response,
                response_capacity);
        }
        return status_response(
            command, airdap_ota_abort(), response, response_capacity);

    case OTA_REBOOT: {
        if (request_length != 1U) {
            return status_response(
                command,
                AIRDAP_OTA_STATUS_INVALID_ARGUMENT,
                response,
                response_capacity);
        }
        const airdap_ota_status_t status = airdap_ota_reboot();
        return status == AIRDAP_OTA_STATUS_OK
            ? 0U
            : status_response(command, status, response, response_capacity);
    }

    default:
        return 0U;
    }
}
