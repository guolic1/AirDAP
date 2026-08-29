#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airdap_swd_protocol.h"

enum {
    AIRDAP_SWD_REQUEST_BITS = 8,
    AIRDAP_SWD_ACK_BITS = 3,
    AIRDAP_SWD_DATA_BITS = 33,
    AIRDAP_SWD_CONNECT_IDLE_CYCLES = 8,
    AIRDAP_SWD_CONNECT_WAIT_RETRIES = 5,
};

static unsigned int parity32(uint32_t value)
{
    unsigned int parity = 0U;

    while (value != 0U) {
        parity ^= value & 1U;
        value >>= 1U;
    }

    return parity;
}

static bool request_is_valid(const airdap_swd_request_t *request)
{
    return request != NULL &&
        (request->port == AIRDAP_SWD_PORT_DP || request->port == AIRDAP_SWD_PORT_AP) &&
        (request->direction == AIRDAP_SWD_WRITE || request->direction == AIRDAP_SWD_READ) &&
        request->address <= 0xCU &&
        (request->address & 0x3U) == 0U;
}

static bool io_is_valid(const airdap_swd_io_t *io)
{
    return io != NULL &&
        io->set_host_output != NULL &&
        io->write_bits != NULL &&
        io->read_bits != NULL &&
        io->turnaround != NULL;
}

uint8_t airdap_swd_encode_request(const airdap_swd_request_t *request)
{
    const unsigned int address_bit_2 = (request->address >> 2U) & 1U;
    const unsigned int address_bit_3 = (request->address >> 3U) & 1U;
    const unsigned int request_parity =
        (unsigned int) request->port ^
        (unsigned int) request->direction ^
        address_bit_2 ^
        address_bit_3;

    return (uint8_t) (
        1U |
        ((unsigned int) request->port << 1U) |
        ((unsigned int) request->direction << 2U) |
        (address_bit_2 << 3U) |
        (address_bit_3 << 4U) |
        (request_parity << 5U) |
        (1U << 7U));
}

static esp_err_t write_idle_cycles(const airdap_swd_io_t *io)
{
    uint8_t remaining = io->idle_cycles;
    while (remaining > 0U) {
        const uint8_t chunk = remaining > 64U ? 64U : remaining;
        esp_err_t error = io->write_bits(io->context, 0U, chunk);
        if (error != ESP_OK) {
            return error;
        }
        remaining -= chunk;
    }
    return ESP_OK;
}

static esp_err_t restore_host_and_idle(const airdap_swd_io_t *io)
{
    esp_err_t error = io->turnaround(io->context, true);
    if (error != ESP_OK) {
        return error;
    }

    return write_idle_cycles(io);
}

static esp_err_t complete_protocol_error(const airdap_swd_io_t *io)
{
    uint64_t ignored = 0U;

    /*
     * An invalid ACK leaves the target's data-phase intent unknown. Keep the
     * host buffer disabled and clock through the complete 32-bit data word and
     * parity bit before taking SWDIO back. The turnaround clocks are supplied
     * by restore_host_and_idle().
     */
    esp_err_t error = io->read_bits(io->context, AIRDAP_SWD_DATA_BITS, &ignored);
    if (error != ESP_OK) {
        (void) restore_host_and_idle(io);
        return error;
    }
    return restore_host_and_idle(io);
}

static esp_err_t complete_error_data_phase(
    const airdap_swd_io_t *io,
    const airdap_swd_request_t *request)
{
    if (!io->data_phase) {
        return restore_host_and_idle(io);
    }

    if (request->direction == AIRDAP_SWD_READ) {
        uint64_t ignored = 0U;
        esp_err_t error = io->read_bits(io->context, AIRDAP_SWD_DATA_BITS, &ignored);
        if (error != ESP_OK) {
            (void) restore_host_and_idle(io);
            return error;
        }
        return restore_host_and_idle(io);
    }

    esp_err_t error = io->turnaround(io->context, true);
    if (error != ESP_OK) {
        return error;
    }
    error = io->write_bits(io->context, 0U, AIRDAP_SWD_DATA_BITS);
    if (error != ESP_OK) {
        return error;
    }
    return write_idle_cycles(io);
}

static esp_err_t transfer_attempt(
    const airdap_swd_io_t *io,
    const airdap_swd_request_t *request,
    uint32_t *data,
    airdap_swd_ack_t *ack)
{
    esp_err_t error = io->write_bits(
        io->context,
        airdap_swd_encode_request(request),
        AIRDAP_SWD_REQUEST_BITS);
    if (error != ESP_OK) {
        return error;
    }

    error = io->turnaround(io->context, false);
    if (error != ESP_OK) {
        return error;
    }

    uint64_t ack_bits = 0U;
    error = io->read_bits(io->context, AIRDAP_SWD_ACK_BITS, &ack_bits);
    if (error != ESP_OK) {
        (void) restore_host_and_idle(io);
        return error;
    }

    *ack = (airdap_swd_ack_t) ack_bits;
    if (*ack != AIRDAP_SWD_ACK_OK &&
        *ack != AIRDAP_SWD_ACK_WAIT &&
        *ack != AIRDAP_SWD_ACK_FAULT) {
        error = complete_protocol_error(io);
        return error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : error;
    }

    if (*ack != AIRDAP_SWD_ACK_OK) {
        return complete_error_data_phase(io, request);
    }

    if (request->direction == AIRDAP_SWD_READ) {
        uint64_t data_bits = 0U;
        error = io->read_bits(io->context, AIRDAP_SWD_DATA_BITS, &data_bits);
        if (error != ESP_OK) {
            (void) restore_host_and_idle(io);
            return error;
        }

        error = restore_host_and_idle(io);
        if (error != ESP_OK) {
            return error;
        }

        const uint32_t value = (uint32_t) data_bits;
        const unsigned int received_parity = (unsigned int) ((data_bits >> 32U) & 1U);
        if (received_parity != parity32(value)) {
            return ESP_ERR_INVALID_CRC;
        }

        *data = value;
        return ESP_OK;
    }

    error = io->turnaround(io->context, true);
    if (error != ESP_OK) {
        return error;
    }

    const uint64_t data_bits =
        (uint64_t) *data |
        ((uint64_t) parity32(*data) << 32U);
    error = io->write_bits(io->context, data_bits, AIRDAP_SWD_DATA_BITS);
    if (error != ESP_OK) {
        return error;
    }

    return write_idle_cycles(io);
}

esp_err_t airdap_swd_protocol_transfer(
    const airdap_swd_io_t *io,
    const airdap_swd_request_t *request,
    uint32_t *data,
    unsigned int wait_retries,
    airdap_swd_ack_t *ack)
{
    if (!io_is_valid(io) || !request_is_valid(request) || data == NULL || ack == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (unsigned int attempt = 0U; attempt <= wait_retries; ++attempt) {
        esp_err_t error = transfer_attempt(io, request, data, ack);
        if (error != ESP_OK || *ack != AIRDAP_SWD_ACK_WAIT) {
            return error;
        }
    }

    return ESP_OK;
}

esp_err_t airdap_swd_protocol_connect(
    const airdap_swd_io_t *io,
    uint32_t *idcode)
{
    if (!io_is_valid(io) || idcode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t error = io->set_host_output(io->context, true);
    if (error != ESP_OK) {
        return error;
    }

    error = io->write_bits(io->context, UINT64_MAX, 64U);
    if (error != ESP_OK) {
        return error;
    }
    error = io->write_bits(io->context, UINT64_C(0xE79E), 16U);
    if (error != ESP_OK) {
        return error;
    }
    error = io->write_bits(io->context, (UINT64_C(1) << 56U) - 1U, 56U);
    if (error != ESP_OK) {
        return error;
    }
    error = io->write_bits(io->context, 0U, AIRDAP_SWD_CONNECT_IDLE_CYCLES);
    if (error != ESP_OK) {
        return error;
    }

    airdap_swd_ack_t ack = AIRDAP_SWD_ACK_NONE;
    error = airdap_swd_protocol_transfer(
        io,
        &(airdap_swd_request_t) {
            .port = AIRDAP_SWD_PORT_DP,
            .direction = AIRDAP_SWD_READ,
            .address = 0x0,
        },
        idcode,
        AIRDAP_SWD_CONNECT_WAIT_RETRIES,
        &ack);
    if (error != ESP_OK) {
        return error;
    }
    if (ack == AIRDAP_SWD_ACK_WAIT) {
        return ESP_ERR_TIMEOUT;
    }
    if (ack != AIRDAP_SWD_ACK_OK) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}
