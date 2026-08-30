#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "airdap_dap_protocol.h"

enum {
    DAP_OK = 0x00,
    DAP_ERROR = 0xFF,

    ID_DAP_INFO = 0x00,
    ID_DAP_HOST_STATUS = 0x01,
    ID_DAP_CONNECT = 0x02,
    ID_DAP_DISCONNECT = 0x03,
    ID_DAP_TRANSFER_CONFIGURE = 0x04,
    ID_DAP_TRANSFER = 0x05,
    ID_DAP_TRANSFER_BLOCK = 0x06,
    ID_DAP_WRITE_ABORT = 0x08,
    ID_DAP_DELAY = 0x09,
    ID_DAP_RESET_TARGET = 0x0A,
    ID_DAP_SWJ_PINS = 0x10,
    ID_DAP_SWJ_CLOCK = 0x11,
    ID_DAP_SWJ_SEQUENCE = 0x12,
    ID_DAP_SWD_CONFIGURE = 0x13,
    ID_DAP_SWD_SEQUENCE = 0x1D,
    ID_AIRDAP_OTA_QUERY = 0x80,
    ID_AIRDAP_OTA_BEGIN = 0x81,
    ID_AIRDAP_OTA_WRITE = 0x82,
    ID_AIRDAP_OTA_COMMIT = 0x83,
    ID_AIRDAP_OTA_ABORT = 0x84,
    ID_AIRDAP_OTA_REBOOT = 0x85,
    ID_DAP_INVALID = 0xFF,

    DAP_INFO_VENDOR = 0x01,
    DAP_INFO_PRODUCT = 0x02,
    DAP_INFO_SERIAL_NUMBER = 0x03,
    DAP_INFO_FIRMWARE_VERSION = 0x04,
    DAP_INFO_PRODUCT_FIRMWARE_VERSION = 0x09,
    DAP_INFO_CAPABILITIES = 0xF0,
    DAP_INFO_PACKET_COUNT = 0xFE,
    DAP_INFO_PACKET_SIZE = 0xFF,

    DAP_TRANSFER_AP = 1U << 0,
    DAP_TRANSFER_READ = 1U << 1,
    DAP_TRANSFER_MATCH_VALUE = 1U << 4,
    DAP_TRANSFER_MATCH_MASK = 1U << 5,
    DAP_TRANSFER_TIMESTAMP = 1U << 7,
    DAP_TRANSFER_OK = 1U << 0,
    DAP_TRANSFER_ERROR = 1U << 3,
    DAP_TRANSFER_MISMATCH = 1U << 4,

    DAP_SWJ_PIN_NRESET = 1U << 7,

    AIRDAP_OTA_WRITE_HEADER_SIZE = 7,
    AIRDAP_OTA_MAX_WRITE_SIZE = 496,
};

_Static_assert(
    AIRDAP_OTA_WRITE_HEADER_SIZE + AIRDAP_OTA_MAX_WRITE_SIZE <=
        AIRDAP_DAP_PACKET_SIZE,
    "OTA write request must fit the DAP packet size");

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

static void write_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t) value;
    data[1] = (uint8_t) (value >> 8U);
}

static void write_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t) value;
    data[1] = (uint8_t) (value >> 8U);
    data[2] = (uint8_t) (value >> 16U);
    data[3] = (uint8_t) (value >> 24U);
}

static size_t invalid_response(uint8_t *response, size_t response_capacity)
{
    if (response != NULL && response_capacity > 0U) {
        response[0] = ID_DAP_INVALID;
        return 1U;
    }
    return 0U;
}

static size_t status_response(
    uint8_t command,
    bool success,
    uint8_t *response,
    size_t response_capacity)
{
    if (response_capacity < 2U) {
        return invalid_response(response, response_capacity);
    }
    response[0] = command;
    response[1] = success ? DAP_OK : DAP_ERROR;
    return 2U;
}

static size_t process_info(
    const airdap_dap_processor_t *processor,
    const uint8_t *request,
    size_t request_length,
    uint8_t *response,
    size_t response_capacity)
{
    if (request_length < 2U || response_capacity < 2U) {
        return invalid_response(response, response_capacity);
    }

    const uint8_t *data = NULL;
    size_t length = 0U;
    static const char vendor[] = "AirDAP";
    static const char product[] = "AirDAP CMSIS-DAP v2";
    static const char protocol_version[] = "2.1.2";
    static const uint8_t capabilities[] = {1U << 0, 1U << 0};
    static const uint8_t packet_count[] = {1U};
    static const uint8_t packet_size[] = {
        (uint8_t) AIRDAP_DAP_PACKET_SIZE,
        (uint8_t) (AIRDAP_DAP_PACKET_SIZE >> 8U),
    };

    switch (request[1]) {
    case DAP_INFO_VENDOR:
        data = (const uint8_t *) vendor;
        length = sizeof(vendor);
        break;
    case DAP_INFO_PRODUCT:
        data = (const uint8_t *) product;
        length = sizeof(product);
        break;
    case DAP_INFO_SERIAL_NUMBER:
        data = (const uint8_t *) processor->serial_number;
        length = strlen(processor->serial_number) + 1U;
        break;
    case DAP_INFO_FIRMWARE_VERSION:
        data = (const uint8_t *) protocol_version;
        length = sizeof(protocol_version);
        break;
    case DAP_INFO_PRODUCT_FIRMWARE_VERSION:
        data = (const uint8_t *) processor->firmware_version;
        length = strlen(processor->firmware_version) + 1U;
        break;
    case DAP_INFO_CAPABILITIES:
        data = capabilities;
        length = sizeof(capabilities);
        break;
    case DAP_INFO_PACKET_COUNT:
        data = packet_count;
        length = sizeof(packet_count);
        break;
    case DAP_INFO_PACKET_SIZE:
        data = packet_size;
        length = sizeof(packet_size);
        break;
    default:
        break;
    }

    if (length > UINT8_MAX || response_capacity < 2U + length) {
        return invalid_response(response, response_capacity);
    }
    response[0] = ID_DAP_INFO;
    response[1] = (uint8_t) length;
    if (length > 0U) {
        memcpy(response + 2U, data, length);
    }
    return 2U + length;
}

static bool transfer_once(
    airdap_dap_processor_t *processor,
    uint8_t transfer_request,
    uint32_t *data,
    uint8_t *status)
{
    const bool ap = (transfer_request & DAP_TRANSFER_AP) != 0U;
    const bool read = (transfer_request & DAP_TRANSFER_READ) != 0U;
    const uint8_t address = transfer_request & 0x0CU;

    if (processor->backend.transfer == NULL ||
        !processor->backend.transfer(
            processor->backend.context,
            ap,
            read,
            address,
            data,
            status)) {
        *status = DAP_TRANSFER_ERROR;
        return false;
    }
    return *status == DAP_TRANSFER_OK;
}

static size_t process_transfer(
    airdap_dap_processor_t *processor,
    const uint8_t *request,
    size_t request_length,
    uint8_t *response,
    size_t response_capacity)
{
    if (request_length < 3U || response_capacity < 3U) {
        return invalid_response(response, response_capacity);
    }

    const size_t framed_length = airdap_dap_request_size(request, request_length);
    if (framed_length == 0U || framed_length == SIZE_MAX) {
        return invalid_response(response, response_capacity);
    }
    size_t required_response = 3U;
    size_t scan = 3U;
    for (uint8_t index = 0U; index < request[2]; ++index) {
        const uint8_t transfer_request = request[scan++];
        const bool read = (transfer_request & DAP_TRANSFER_READ) != 0U;
        const bool match_value = (transfer_request & DAP_TRANSFER_MATCH_VALUE) != 0U;
        if (!read || match_value) {
            scan += 4U;
        }
        if (read && !match_value) {
            required_response +=
                (transfer_request & DAP_TRANSFER_TIMESTAMP) != 0U ? 8U : 4U;
        }
    }
    if (required_response > response_capacity || scan != framed_length) {
        return invalid_response(response, response_capacity);
    }

    response[0] = ID_DAP_TRANSFER;
    response[1] = 0U;
    response[2] = 0U;
    if (processor->selected_port != AIRDAP_DAP_PORT_SWD) {
        return 3U;
    }

    size_t input = 3U;
    size_t output = 3U;
    uint8_t completed = 0U;
    uint8_t status = DAP_TRANSFER_OK;
    bool pending_write = false;

    for (uint8_t index = 0U; index < request[2]; ++index) {
        if (input >= request_length) {
            return invalid_response(response, response_capacity);
        }
        const uint8_t transfer_request = request[input++];
        const bool read = (transfer_request & DAP_TRANSFER_READ) != 0U;
        const bool match_value = (transfer_request & DAP_TRANSFER_MATCH_VALUE) != 0U;
        const bool match_mask = (transfer_request & DAP_TRANSFER_MATCH_MASK) != 0U;
        const bool timestamp = (transfer_request & DAP_TRANSFER_TIMESTAMP) != 0U;
        uint32_t data = 0U;

        if (match_mask) {
            if (read || input + 4U > request_length) {
                return invalid_response(response, response_capacity);
            }
            processor->match_mask = read_u32(request + input);
            input += 4U;
            ++completed;
            continue;
        }

        if (!read || match_value) {
            if (input + 4U > request_length) {
                return invalid_response(response, response_capacity);
            }
            data = read_u32(request + input);
            input += 4U;
        }

        if (match_value) {
            bool matched = false;
            for (uint32_t attempt = 0U; attempt <= processor->match_retries; ++attempt) {
                uint32_t actual = 0U;
                if (!transfer_once(processor, transfer_request, &actual, &status)) {
                    break;
                }
                if ((actual & processor->match_mask) == (data & processor->match_mask)) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                if (status == DAP_TRANSFER_OK) {
                    status |= DAP_TRANSFER_MISMATCH;
                }
                break;
            }
            ++completed;
            continue;
        }

        if (!transfer_once(processor, transfer_request, &data, &status)) {
            break;
        }
        ++completed;
        pending_write = !read;

        if (read) {
            const size_t bytes_needed = timestamp ? 8U : 4U;
            if (output + bytes_needed > response_capacity) {
                return invalid_response(response, response_capacity);
            }
            if (timestamp) {
                write_u32(response + output, 0U);
                output += 4U;
            }
            write_u32(response + output, data);
            output += 4U;
        }
    }

    if (pending_write && status == DAP_TRANSFER_OK) {
        uint32_t ignored = 0U;
        (void) transfer_once(
            processor,
            DAP_TRANSFER_READ | 0x0CU,
            &ignored,
            &status);
    }

    response[1] = completed;
    response[2] = status;
    return output;
}

static size_t process_transfer_block(
    airdap_dap_processor_t *processor,
    const uint8_t *request,
    size_t request_length,
    uint8_t *response,
    size_t response_capacity)
{
    if (request_length < 5U || response_capacity < 4U) {
        return invalid_response(response, response_capacity);
    }

    const uint16_t count = read_u16(request + 2U);
    const uint8_t transfer_request = request[4];
    const bool read = (transfer_request & DAP_TRANSFER_READ) != 0U;
    if ((read && count > (response_capacity - 4U) / 4U) ||
        (!read && count > (request_length - 5U) / 4U)) {
        return invalid_response(response, response_capacity);
    }
    size_t input = 5U;
    size_t output = 4U;
    uint16_t completed = 0U;
    uint8_t status = DAP_TRANSFER_OK;

    response[0] = ID_DAP_TRANSFER_BLOCK;
    if (processor->selected_port == AIRDAP_DAP_PORT_SWD) {
        for (uint16_t index = 0U; index < count; ++index) {
            uint32_t data = 0U;
            if (!read) {
                if (input + 4U > request_length) {
                    return invalid_response(response, response_capacity);
                }
                data = read_u32(request + input);
                input += 4U;
            } else if (output + 4U > response_capacity) {
                return invalid_response(response, response_capacity);
            }

            if (!transfer_once(processor, transfer_request, &data, &status)) {
                break;
            }
            ++completed;
            if (read) {
                write_u32(response + output, data);
                output += 4U;
            }
        }
        if (!read && completed > 0U && status == DAP_TRANSFER_OK) {
            uint32_t ignored = 0U;
            (void) transfer_once(
                processor,
                DAP_TRANSFER_READ | 0x0CU,
                &ignored,
                &status);
        }
    } else {
        status = 0U;
    }

    write_u16(response + 1U, completed);
    response[3] = status;
    return output;
}

static size_t process_swd_sequence(
    airdap_dap_processor_t *processor,
    const uint8_t *request,
    size_t request_length,
    uint8_t *response,
    size_t response_capacity)
{
    if (request_length < 2U || response_capacity < 2U) {
        return invalid_response(response, response_capacity);
    }

    size_t input = 2U;
    size_t output = 2U;
    bool success = processor->selected_port == AIRDAP_DAP_PORT_SWD;

    size_t required_response = 2U;
    size_t scan = 2U;
    for (uint8_t sequence = 0U; sequence < request[1]; ++sequence) {
        if (scan >= request_length) {
            return invalid_response(response, response_capacity);
        }
        const uint8_t info = request[scan++];
        const size_t bit_count = (info & 0x3FU) == 0U ? 64U : info & 0x3FU;
        const size_t byte_count = (bit_count + 7U) / 8U;
        if ((info & 0x80U) != 0U) {
            required_response += byte_count;
        } else {
            scan += byte_count;
            if (scan > request_length) {
                return invalid_response(response, response_capacity);
            }
        }
    }
    if (required_response > response_capacity) {
        return invalid_response(response, response_capacity);
    }

    for (uint8_t sequence = 0U; sequence < request[1]; ++sequence) {
        if (input >= request_length) {
            return invalid_response(response, response_capacity);
        }
        const uint8_t info = request[input++];
        const bool input_sequence = (info & 0x80U) != 0U;
        const size_t bit_count = (info & 0x3FU) == 0U ? 64U : info & 0x3FU;
        const size_t byte_count = (bit_count + 7U) / 8U;

        if (input_sequence) {
            if (output + byte_count > response_capacity) {
                return invalid_response(response, response_capacity);
            }
            memset(response + output, 0, byte_count);
            if (success && (processor->backend.read_sequence == NULL ||
                !processor->backend.read_sequence(
                    processor->backend.context,
                    response + output,
                    bit_count))) {
                success = false;
            }
            output += byte_count;
        } else {
            if (input + byte_count > request_length) {
                return invalid_response(response, response_capacity);
            }
            if (success && (processor->backend.write_sequence == NULL ||
                !processor->backend.write_sequence(
                    processor->backend.context,
                    request + input,
                    bit_count))) {
                success = false;
            }
            input += byte_count;
        }
    }

    response[0] = ID_DAP_SWD_SEQUENCE;
    response[1] = success ? DAP_OK : DAP_ERROR;
    return output;
}

void airdap_dap_processor_init(
    airdap_dap_processor_t *processor,
    const airdap_dap_backend_t *backend,
    const char *serial_number,
    const char *firmware_version)
{
    if (processor == NULL) {
        return;
    }
    memset(processor, 0, sizeof(*processor));
    if (backend != NULL) {
        processor->backend = *backend;
    }
    processor->serial_number = serial_number != NULL ? serial_number : "";
    processor->firmware_version = firmware_version != NULL
        ? firmware_version
        : "";
    processor->match_mask = UINT32_MAX;
}

size_t airdap_dap_request_size(
    const uint8_t *request,
    size_t available_length)
{
    if (request == NULL || available_length == 0U) {
        return 0U;
    }

    size_t length;
    switch (request[0]) {
    case ID_DAP_INFO:
    case ID_DAP_CONNECT:
    case ID_DAP_SWD_CONFIGURE:
        return available_length >= 2U ? 2U : 0U;
    case ID_DAP_HOST_STATUS:
    case ID_DAP_DELAY:
        return available_length >= 3U ? 3U : 0U;
    case ID_DAP_DISCONNECT:
    case ID_DAP_RESET_TARGET:
    case ID_AIRDAP_OTA_QUERY:
    case ID_AIRDAP_OTA_COMMIT:
    case ID_AIRDAP_OTA_ABORT:
    case ID_AIRDAP_OTA_REBOOT:
        return 1U;
    case ID_DAP_TRANSFER_CONFIGURE:
    case ID_DAP_WRITE_ABORT:
        return available_length >= 6U ? 6U : 0U;
    case ID_DAP_SWJ_PINS:
        return available_length >= 7U ? 7U : 0U;
    case ID_DAP_SWJ_CLOCK:
    case ID_AIRDAP_OTA_BEGIN:
        return available_length >= 5U ? 5U : 0U;
    case ID_AIRDAP_OTA_WRITE:
        if (available_length < AIRDAP_OTA_WRITE_HEADER_SIZE) {
            return 0U;
        }
        length = read_u16(request + 5U);
        if (length > AIRDAP_OTA_MAX_WRITE_SIZE) {
            return SIZE_MAX;
        }
        length += AIRDAP_OTA_WRITE_HEADER_SIZE;
        return available_length >= length ? length : 0U;
    case ID_DAP_SWJ_SEQUENCE:
        if (available_length < 2U) {
            return 0U;
        }
        length = 2U + (((request[1] == 0U ? 256U : request[1]) + 7U) / 8U);
        if (length > AIRDAP_DAP_PACKET_SIZE) {
            return SIZE_MAX;
        }
        return available_length >= length ? length : 0U;
    case ID_DAP_TRANSFER_BLOCK:
        if (available_length < 5U) {
            return 0U;
        }
        length = 5U;
        if ((request[4] & DAP_TRANSFER_READ) == 0U) {
            const size_t count = read_u16(request + 2U);
            if (count > (SIZE_MAX - length) / 4U) {
                return SIZE_MAX;
            }
            length += count * 4U;
        }
        if (length > AIRDAP_DAP_PACKET_SIZE) {
            return SIZE_MAX;
        }
        return available_length >= length ? length : 0U;
    case ID_DAP_TRANSFER:
        if (available_length < 3U) {
            return 0U;
        }
        length = 3U;
        for (uint8_t index = 0U; index < request[2]; ++index) {
            if (available_length <= length) {
                return 0U;
            }
            const uint8_t transfer_request = request[length++];
            if ((transfer_request & DAP_TRANSFER_READ) == 0U ||
                (transfer_request & DAP_TRANSFER_MATCH_VALUE) != 0U) {
                if (length > SIZE_MAX - 4U) {
                    return SIZE_MAX;
                }
                length += 4U;
            }
            if (length > AIRDAP_DAP_PACKET_SIZE) {
                return SIZE_MAX;
            }
            if (available_length < length) {
                return 0U;
            }
        }
        return length;
    case ID_DAP_SWD_SEQUENCE:
        if (available_length < 2U) {
            return 0U;
        }
        length = 2U;
        for (uint8_t index = 0U; index < request[1]; ++index) {
            if (available_length <= length) {
                return 0U;
            }
            const uint8_t sequence_info = request[length++];
            if ((sequence_info & 0x80U) == 0U) {
                const size_t bit_count = (sequence_info & 0x3FU) == 0U
                    ? 64U
                    : sequence_info & 0x3FU;
                length += (bit_count + 7U) / 8U;
                if (length > AIRDAP_DAP_PACKET_SIZE) {
                    return SIZE_MAX;
                }
                if (available_length < length) {
                    return 0U;
                }
            }
        }
        return length;
    default:
        return 1U;
    }
}

size_t airdap_dap_process_packet(
    airdap_dap_processor_t *processor,
    const uint8_t *request,
    size_t request_length,
    uint8_t *response,
    size_t response_capacity)
{
    if (processor == NULL || request == NULL || request_length == 0U ||
        response == NULL || response_capacity == 0U) {
        return 0U;
    }
    if (response_capacity > AIRDAP_DAP_PACKET_SIZE) {
        response_capacity = AIRDAP_DAP_PACKET_SIZE;
    }

    switch (request[0]) {
    case ID_DAP_INFO:
        return process_info(processor, request, request_length, response, response_capacity);

    case ID_DAP_HOST_STATUS:
        if (request_length < 3U || request[1] > 1U) {
            return invalid_response(response, response_capacity);
        }
        if (processor->backend.host_status != NULL) {
            processor->backend.host_status(
                processor->backend.context,
                request[1],
                (request[2] & 1U) != 0U);
        }
        return status_response(request[0], true, response, response_capacity);

    case ID_DAP_CONNECT: {
        if (request_length < 2U || response_capacity < 2U) {
            return invalid_response(response, response_capacity);
        }
        const bool supported_port = request[1] == 0U || request[1] == AIRDAP_DAP_PORT_SWD;
        const bool connected = supported_port && processor->backend.connect != NULL &&
            processor->backend.connect(processor->backend.context);
        if (!connected && processor->backend.disconnect != NULL) {
            processor->backend.disconnect(processor->backend.context);
        }
        processor->selected_port = connected ? AIRDAP_DAP_PORT_SWD : AIRDAP_DAP_PORT_DISABLED;
        response[0] = request[0];
        response[1] = processor->selected_port;
        return 2U;
    }

    case ID_DAP_DISCONNECT:
        if (processor->backend.disconnect != NULL) {
            processor->backend.disconnect(processor->backend.context);
        }
        processor->selected_port = AIRDAP_DAP_PORT_DISABLED;
        return status_response(request[0], true, response, response_capacity);

    case ID_DAP_TRANSFER_CONFIGURE: {
        if (request_length < 6U) {
            return invalid_response(response, response_capacity);
        }
        processor->match_retries = read_u16(request + 4U);
        const bool success = processor->backend.configure_transfer != NULL &&
            processor->backend.configure_transfer(
                processor->backend.context,
                request[1],
                read_u16(request + 2U));
        return status_response(request[0], success, response, response_capacity);
    }

    case ID_DAP_TRANSFER:
        return process_transfer(
            processor, request, request_length, response, response_capacity);

    case ID_DAP_TRANSFER_BLOCK:
        return process_transfer_block(
            processor, request, request_length, response, response_capacity);

    case ID_DAP_WRITE_ABORT: {
        if (request_length < 6U) {
            return invalid_response(response, response_capacity);
        }
        uint32_t data = read_u32(request + 2U);
        uint8_t status = 0U;
        const bool success = processor->selected_port == AIRDAP_DAP_PORT_SWD &&
            transfer_once(processor, 0U, &data, &status);
        return status_response(request[0], success, response, response_capacity);
    }

    case ID_DAP_DELAY:
        if (request_length < 3U) {
            return invalid_response(response, response_capacity);
        }
        if (processor->backend.delay_us != NULL) {
            processor->backend.delay_us(
                processor->backend.context,
                read_u16(request + 1U));
        }
        return status_response(request[0], true, response, response_capacity);

    case ID_DAP_RESET_TARGET: {
        if (response_capacity < 3U) {
            return invalid_response(response, response_capacity);
        }
        const bool success = processor->backend.reset_target != NULL &&
            processor->backend.reset_target(processor->backend.context);
        response[0] = request[0];
        response[1] = success ? DAP_OK : DAP_ERROR;
        response[2] = 0U;
        return 3U;
    }

    case ID_DAP_SWJ_PINS: {
        if (request_length < 7U || response_capacity < 2U) {
            return invalid_response(response, response_capacity);
        }
        uint8_t pins = 0U;
        const bool success = processor->backend.swj_pins != NULL &&
            processor->backend.swj_pins(
                processor->backend.context,
                request[1],
                request[2],
                read_u32(request + 3U),
                &pins);
        response[0] = request[0];
        response[1] = success ? pins : 0U;
        return 2U;
    }

    case ID_DAP_SWJ_CLOCK: {
        if (request_length < 5U) {
            return invalid_response(response, response_capacity);
        }
        const uint32_t clock_hz = read_u32(request + 1U);
        const bool success = clock_hz != 0U && processor->backend.set_clock != NULL &&
            processor->backend.set_clock(processor->backend.context, clock_hz);
        return status_response(request[0], success, response, response_capacity);
    }

    case ID_DAP_SWJ_SEQUENCE: {
        if (request_length < 2U) {
            return invalid_response(response, response_capacity);
        }
        const size_t bit_count = request[1] == 0U ? 256U : request[1];
        const size_t byte_count = (bit_count + 7U) / 8U;
        const bool success = request_length >= 2U + byte_count &&
            processor->selected_port == AIRDAP_DAP_PORT_SWD &&
            processor->backend.write_sequence != NULL &&
            processor->backend.write_sequence(
                processor->backend.context,
                request + 2U,
                bit_count);
        return status_response(request[0], success, response, response_capacity);
    }

    case ID_DAP_SWD_CONFIGURE: {
        if (request_length < 2U) {
            return invalid_response(response, response_capacity);
        }
        const bool success = processor->backend.configure_swd != NULL &&
            processor->backend.configure_swd(
                processor->backend.context,
                (request[1] & 0x03U) + 1U,
                (request[1] & 0x04U) != 0U);
        return status_response(request[0], success, response, response_capacity);
    }

    case ID_DAP_SWD_SEQUENCE:
        return process_swd_sequence(
            processor, request, request_length, response, response_capacity);

    case ID_AIRDAP_OTA_QUERY:
    case ID_AIRDAP_OTA_BEGIN:
    case ID_AIRDAP_OTA_WRITE:
    case ID_AIRDAP_OTA_COMMIT:
    case ID_AIRDAP_OTA_ABORT:
    case ID_AIRDAP_OTA_REBOOT:
        if (processor->backend.vendor_command == NULL) {
            return invalid_response(response, response_capacity);
        }
        return processor->backend.vendor_command(
            processor->backend.context,
            processor->selected_port != AIRDAP_DAP_PORT_DISABLED,
            request,
            request_length,
            response,
            response_capacity);

    default:
        return invalid_response(response, response_capacity);
    }
}
