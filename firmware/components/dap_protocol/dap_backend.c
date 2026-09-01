#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airdap_board.h"
#include "airdap_dap.h"
#include "airdap_dap_ownership.h"
#include "airdap_dap_ota.h"
#include "airdap_dap_protocol.h"
#include "airdap_mode_state.h"
#include "airdap_ota.h"
#include "airdap_swd.h"
#include "esp_rom_sys.h"

enum {
    DAP_TRANSFER_ERROR = 1U << 3,
    DAP_SWJ_PIN_SWCLK = 1U << 0,
    DAP_SWJ_PIN_SWDIO = 1U << 1,
    DAP_SWJ_PIN_NRESET = 1U << 7,
    DAP_TRANSPORT_COUNT = 2,
};

typedef struct {
    airdap_dap_owner_t owner;
    airdap_dap_processor_t processor;
    airdap_dap_ownership_claim_t claim;
} dap_transport_context_t;

static dap_transport_context_t transports[DAP_TRANSPORT_COUNT];
static bool initialized;
static bool target_reset_released = true;
static const uint8_t swd_line_reset[] = {
    0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU,
};

static bool ownership_line_reset(void *context)
{
    (void) context;
    return airdap_swd_write_sequence(
        swd_line_reset,
        sizeof(swd_line_reset) * 8U) == ESP_OK;
}

static bool ownership_release_pins(void *context)
{
    (void) context;
    const esp_err_t swdio_error = airdap_swd_set_io_state(false);
    const esp_err_t reset_error = airdap_target_reset_set_asserted(false);
    if (reset_error == ESP_OK) {
        target_reset_released = true;
    }
    return swdio_error == ESP_OK && reset_error == ESP_OK;
}

static dap_transport_context_t *transport_context(
    airdap_dap_owner_t owner)
{
    switch (owner) {
    case AIRDAP_DAP_OWNER_USB:
        return &transports[0];
    case AIRDAP_DAP_OWNER_NETWORK:
        return &transports[1];
    default:
        return NULL;
    }
}

static bool begin_operation(
    dap_transport_context_t *transport,
    airdap_dap_ownership_operation_t *operation)
{
    return airdap_mode_state_dap_operation_begin(
        transport->owner,
        true,
        &transport->claim,
        operation) == AIRDAP_MODE_DAP_ALLOWED;
}

static bool backend_connect(void *context)
{
    dap_transport_context_t *transport = context;
    if (!airdap_ota_debug_allowed()) {
        return false;
    }
    /* NETWORK packets reach this backend only through an authenticated
     * dap_service session. */
    return airdap_mode_state_dap_acquire(
        transport->owner,
        true,
        &transport->claim) == AIRDAP_MODE_DAP_ALLOWED;
}

static void backend_disconnect(void *context)
{
    dap_transport_context_t *transport = context;
    (void) airdap_dap_ownership_release(&transport->claim);
    if (transport->owner == AIRDAP_DAP_OWNER_USB) {
        airdap_ota_handle_disconnect();
    }
}

static bool backend_set_clock(void *context, uint32_t clock_hz)
{
    dap_transport_context_t *transport = context;
    airdap_dap_ownership_operation_t operation = {0};
    if (!begin_operation(transport, &operation)) {
        return false;
    }
    const bool success = airdap_swd_set_clock(clock_hz) == ESP_OK;
    airdap_dap_ownership_operation_end(&operation);
    return success;
}

static bool backend_configure_transfer(
    void *context,
    uint8_t idle_cycles,
    uint16_t wait_retries)
{
    dap_transport_context_t *transport = context;
    airdap_dap_ownership_operation_t operation = {0};
    if (!begin_operation(transport, &operation)) {
        return false;
    }
    const bool success =
        airdap_swd_configure_transfer(idle_cycles, wait_retries) == ESP_OK;
    airdap_dap_ownership_operation_end(&operation);
    return success;
}

static bool backend_configure_swd(
    void *context,
    uint8_t turnaround_cycles,
    bool data_phase)
{
    dap_transport_context_t *transport = context;
    airdap_dap_ownership_operation_t operation = {0};
    if (!begin_operation(transport, &operation)) {
        return false;
    }
    const bool success =
        airdap_swd_configure_bus(turnaround_cycles, data_phase) == ESP_OK;
    airdap_dap_ownership_operation_end(&operation);
    return success;
}

static bool backend_transfer(
    void *context,
    bool ap,
    bool read,
    uint8_t address,
    uint32_t *data,
    uint8_t *status)
{
    dap_transport_context_t *transport = context;
    airdap_dap_ownership_operation_t operation = {0};
    if (!begin_operation(transport, &operation)) {
        *status = DAP_TRANSFER_ERROR;
        return false;
    }
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
    airdap_dap_ownership_operation_end(&operation);
    return error == ESP_OK;
}

static bool backend_write_sequence(
    void *context,
    const uint8_t *data,
    size_t bit_count)
{
    dap_transport_context_t *transport = context;
    airdap_dap_ownership_operation_t operation = {0};
    if (!begin_operation(transport, &operation)) {
        return false;
    }
    const bool success = airdap_swd_write_sequence(data, bit_count) == ESP_OK;
    airdap_dap_ownership_operation_end(&operation);
    return success;
}

static bool backend_read_sequence(
    void *context,
    uint8_t *data,
    size_t bit_count)
{
    dap_transport_context_t *transport = context;
    airdap_dap_ownership_operation_t operation = {0};
    if (!begin_operation(transport, &operation)) {
        return false;
    }
    const bool success = airdap_swd_read_sequence(data, bit_count) == ESP_OK;
    airdap_dap_ownership_operation_end(&operation);
    return success;
}

static bool swj_pins_owned(
    uint8_t value,
    uint8_t select,
    uint32_t wait_us,
    uint8_t *pins)
{
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

static bool backend_swj_pins(
    void *context,
    uint8_t value,
    uint8_t select,
    uint32_t wait_us,
    uint8_t *pins)
{
    dap_transport_context_t *transport = context;
    airdap_dap_ownership_operation_t operation = {0};
    if (!begin_operation(transport, &operation)) {
        return false;
    }
    const bool success = swj_pins_owned(value, select, wait_us, pins);
    airdap_dap_ownership_operation_end(&operation);
    return success;
}

static bool reset_target_owned(void)
{
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

static bool backend_reset_target(void *context)
{
    dap_transport_context_t *transport = context;
    airdap_dap_ownership_operation_t operation = {0};
    if (!begin_operation(transport, &operation)) {
        return false;
    }
    const bool success = reset_target_owned();
    airdap_dap_ownership_operation_end(&operation);
    return success;
}

static void backend_delay_us(void *context, uint16_t delay_us)
{
    (void) context;
    esp_rom_delay_us(delay_us);
}

static size_t backend_vendor_command(
    void *context,
    bool debug_connected,
    const uint8_t *request,
    size_t request_length,
    uint8_t *response,
    size_t response_capacity)
{
    (void) context;
    (void) debug_connected;
    return airdap_dap_ota_process(
        airdap_dap_ownership_current() != AIRDAP_DAP_OWNER_NONE,
        request,
        request_length,
        response,
        response_capacity);
}

esp_err_t airdap_dap_init(
    const char *serial_number,
    const char *firmware_version)
{
    if (serial_number == NULL || firmware_version == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const airdap_dap_ownership_backend_t ownership = {
        .line_reset = ownership_line_reset,
        .release_pins = ownership_release_pins,
    };
    if (airdap_dap_ownership_initialize(&ownership) !=
        AIRDAP_DAP_OWNERSHIP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    transports[0].owner = AIRDAP_DAP_OWNER_USB;
    transports[1].owner = AIRDAP_DAP_OWNER_NETWORK;
    for (size_t index = 0U; index < DAP_TRANSPORT_COUNT; ++index) {
        const airdap_dap_backend_t backend = {
            .context = &transports[index],
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
            .vendor_command = backend_vendor_command,
        };
        airdap_dap_processor_init(
            &transports[index].processor,
            &backend,
            serial_number,
            firmware_version);
    }
    initialized = true;
    return ESP_OK;
}

size_t airdap_dap_process(
    airdap_dap_owner_t owner,
    const uint8_t *request,
    size_t request_length,
    uint8_t *response,
    size_t response_capacity)
{
    dap_transport_context_t *transport = transport_context(owner);
    if (!initialized || transport == NULL) {
        return 0U;
    }
    return airdap_dap_process_packet(
        &transport->processor,
        request,
        request_length,
        response,
        response_capacity);
}

void airdap_dap_session_closed(airdap_dap_owner_t owner)
{
    dap_transport_context_t *transport = transport_context(owner);
    if (!initialized || transport == NULL) {
        return;
    }
    uint8_t response[2];
    const uint8_t disconnect[] = {0x03U};
    (void) airdap_dap_process_packet(
        &transport->processor,
        disconnect,
        sizeof(disconnect),
        response,
        sizeof(response));
}
