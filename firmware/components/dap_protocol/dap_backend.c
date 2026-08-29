#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airdap_board.h"
#include "airdap_dap.h"
#include "airdap_dap_protocol.h"
#include "airdap_swd.h"
#include "esp_rom_sys.h"

enum {
    DAP_TRANSFER_ERROR = 1U << 3,
    DAP_SWJ_PIN_SWCLK = 1U << 0,
    DAP_SWJ_PIN_SWDIO = 1U << 1,
    DAP_SWJ_PIN_NRESET = 1U << 7,
};

static airdap_dap_processor_t processor;
static bool initialized;
static bool target_reset_released = true;

static bool backend_connect(void *context)
{
    (void) context;
    return airdap_swd_set_io_state(true) == ESP_OK;
}

static void backend_disconnect(void *context)
{
    (void) context;
    (void) airdap_swd_set_io_state(false);
    (void) airdap_target_reset_set_asserted(false);
    target_reset_released = true;
}

static bool backend_set_clock(void *context, uint32_t clock_hz)
{
    (void) context;
    return airdap_swd_set_clock(clock_hz) == ESP_OK;
}

static bool backend_configure_transfer(
    void *context,
    uint8_t idle_cycles,
    uint16_t wait_retries)
{
    (void) context;
    return airdap_swd_configure_transfer(idle_cycles, wait_retries) == ESP_OK;
}

static bool backend_configure_swd(
    void *context,
    uint8_t turnaround_cycles,
    bool data_phase)
{
    (void) context;
    return airdap_swd_configure_bus(turnaround_cycles, data_phase) == ESP_OK;
}

static bool backend_transfer(
    void *context,
    bool ap,
    bool read,
    uint8_t address,
    uint32_t *data,
    uint8_t *status)
{
    (void) context;
    airdap_swd_ack_t ack = AIRDAP_SWD_ACK_NONE;
    esp_err_t error;

    if (read) {
        error = ap
            ? airdap_swd_read_ap(address, data, &ack)
            : airdap_swd_read_dp(address, data, &ack);
    } else {
        error = ap
            ? airdap_swd_write_ap(address, *data, &ack)
            : airdap_swd_write_dp(address, *data, &ack);
    }

    *status = error == ESP_OK ? (uint8_t) ack : DAP_TRANSFER_ERROR;
    return error == ESP_OK;
}

static bool backend_write_sequence(
    void *context,
    const uint8_t *data,
    size_t bit_count)
{
    (void) context;
    return airdap_swd_write_sequence(data, bit_count) == ESP_OK;
}

static bool backend_read_sequence(
    void *context,
    uint8_t *data,
    size_t bit_count)
{
    (void) context;
    return airdap_swd_read_sequence(data, bit_count) == ESP_OK;
}

static bool backend_swj_pins(
    void *context,
    uint8_t value,
    uint8_t select,
    uint32_t wait_us,
    uint8_t *pins)
{
    (void) context;

    if ((select & DAP_SWJ_PIN_NRESET) != 0U) {
        target_reset_released = (value & DAP_SWJ_PIN_NRESET) != 0U;
        if (airdap_target_reset_set_asserted(!target_reset_released) != ESP_OK) {
            return false;
        }
    }

    const uint8_t swd_select = select &
        (DAP_SWJ_PIN_SWCLK | DAP_SWJ_PIN_SWDIO);
    if (swd_select != 0U) {
        uint8_t swd_pins = 0U;
        if (airdap_swd_drive_pins(
            value,
            swd_select,
            wait_us,
            &swd_pins) != ESP_OK) {
            return false;
        }
        *pins = swd_pins;
        wait_us = 0U;
    } else {
        *pins = 0U;
    }

    if (wait_us > 0U) {
        esp_rom_delay_us(wait_us > 3000000U ? 3000000U : wait_us);
    }
    if (target_reset_released) {
        *pins |= DAP_SWJ_PIN_NRESET;
    }
    return true;
}

static bool backend_reset_target(void *context)
{
    (void) context;
    if (airdap_target_reset_set_asserted(true) != ESP_OK) {
        return false;
    }
    target_reset_released = false;
    esp_rom_delay_us(1000U);
    if (airdap_target_reset_set_asserted(false) != ESP_OK) {
        return false;
    }
    target_reset_released = true;
    return true;
}

static void backend_delay_us(void *context, uint16_t delay_us)
{
    (void) context;
    esp_rom_delay_us(delay_us);
}

esp_err_t airdap_dap_init(const char *serial_number)
{
    if (serial_number == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const airdap_dap_backend_t backend = {
        .connect = backend_connect,
        .disconnect = backend_disconnect,
        .set_clock = backend_set_clock,
        .configure_transfer = backend_configure_transfer,
        .configure_swd = backend_configure_swd,
        .transfer = backend_transfer,
        .write_sequence = backend_write_sequence,
        .read_sequence = backend_read_sequence,
        .swj_pins = backend_swj_pins,
        .reset_target = backend_reset_target,
        .delay_us = backend_delay_us,
    };
    airdap_dap_processor_init(&processor, &backend, serial_number);
    initialized = true;
    return ESP_OK;
}

size_t airdap_dap_process(
    const uint8_t *request,
    size_t request_length,
    uint8_t *response,
    size_t response_capacity)
{
    if (!initialized) {
        return 0U;
    }
    return airdap_dap_process_packet(
        &processor,
        request,
        request_length,
        response,
        response_capacity);
}
