#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_dap.h"
#include "airdap_dap_ownership.h"
#include "airdap_dap_protocol.h"
#include "airdap_swd.h"
#include "esp_err.h"

static bool ota_debug_allowed = true;
static esp_err_t line_reset_result = ESP_OK;
static unsigned line_reset_calls;
static unsigned release_io_calls;
static unsigned release_reset_calls;
static unsigned target_reset_calls;
static unsigned ota_disconnect_calls;
static bool ota_receiving;
static size_t last_line_reset_bits;
static uint8_t last_line_reset[8];
static bool revoke_during_sequence;
static airdap_dap_ownership_result_t sequence_revoke_result;

bool airdap_ota_debug_allowed(void)
{
    return ota_debug_allowed;
}

void airdap_ota_handle_disconnect(void)
{
    ++ota_disconnect_calls;
    ota_receiving = false;
}

esp_err_t airdap_target_reset_set_asserted(bool asserted)
{
    ++target_reset_calls;
    if (!asserted) {
        ++release_reset_calls;
    }
    return ESP_OK;
}

esp_err_t airdap_swd_set_clock(uint32_t clock_hz)
{
    return clock_hz == 0U ? ESP_ERR_INVALID_ARG : ESP_OK;
}

esp_err_t airdap_swd_configure_transfer(
    uint8_t idle_cycles,
    unsigned int wait_retries)
{
    (void) idle_cycles;
    (void) wait_retries;
    return ESP_OK;
}

esp_err_t airdap_swd_configure_bus(
    uint8_t turnaround_cycles,
    bool data_phase)
{
    (void) turnaround_cycles;
    (void) data_phase;
    return ESP_OK;
}

esp_err_t airdap_swd_set_io_state(bool host_output)
{
    if (!host_output) {
        ++release_io_calls;
    }
    return ESP_OK;
}

esp_err_t airdap_swd_write_sequence(
    const uint8_t *data,
    size_t bit_count)
{
    assert(data != NULL);
    ++line_reset_calls;
    const size_t byte_count = (bit_count + 7U) / 8U;
    assert(byte_count <= sizeof(last_line_reset));
    bool is_line_reset = bit_count == 64U;
    for (size_t index = 0U; index < byte_count && is_line_reset; ++index) {
        is_line_reset = data[index] == 0xFFU;
    }
    if (is_line_reset) {
        last_line_reset_bits = bit_count;
        memcpy(last_line_reset, data, byte_count);
    } else {
        --line_reset_calls;
    }
    if (revoke_during_sequence) {
        sequence_revoke_result = airdap_dap_ownership_revoke();
    }
    return line_reset_result;
}

esp_err_t airdap_swd_read_sequence(uint8_t *data, size_t bit_count)
{
    assert(data != NULL && bit_count > 0U);
    memset(data, 0, (bit_count + 7U) / 8U);
    return ESP_OK;
}

esp_err_t airdap_swd_drive_pins(
    uint8_t value,
    uint8_t select,
    uint32_t wait_us,
    uint8_t *pins)
{
    (void) wait_us;
    assert(pins != NULL);
    *pins = value & select;
    return ESP_OK;
}

static esp_err_t fake_transfer(uint32_t *data, airdap_swd_ack_t *ack)
{
    assert(data != NULL && ack != NULL);
    *ack = AIRDAP_SWD_ACK_OK;
    return ESP_OK;
}

esp_err_t airdap_swd_read_dp(
    uint8_t address,
    uint32_t *data,
    airdap_swd_ack_t *ack)
{
    (void) address;
    return fake_transfer(data, ack);
}

esp_err_t airdap_swd_write_dp(
    uint8_t address,
    uint32_t data,
    airdap_swd_ack_t *ack)
{
    (void) address;
    return fake_transfer(&data, ack);
}

esp_err_t airdap_swd_read_ap(
    uint8_t address,
    uint32_t *data,
    airdap_swd_ack_t *ack)
{
    (void) address;
    return fake_transfer(data, ack);
}

esp_err_t airdap_swd_write_ap(
    uint8_t address,
    uint32_t data,
    airdap_swd_ack_t *ack)
{
    (void) address;
    return fake_transfer(&data, ack);
}

void esp_rom_delay_us(uint32_t delay_us)
{
    (void) delay_us;
}

size_t airdap_dap_ota_process(
    bool debug_connected,
    const uint8_t *request,
    size_t request_length,
    uint8_t *response,
    size_t response_capacity)
{
    assert(request != NULL && request_length > 0U);
    assert(response != NULL && response_capacity >= 2U);
    if (request[0] == 0x81U) {
        assert(!debug_connected);
        ota_receiving = true;
    }
    response[0] = request[0];
    response[1] = 0U;
    return 2U;
}

static size_t process_owner(
    airdap_dap_owner_t owner,
    const uint8_t *request,
    size_t request_length,
    uint8_t *response)
{
    memset(response, 0xCC, AIRDAP_DAP_PACKET_SIZE);
    return airdap_dap_process(
        owner,
        request,
        request_length,
        response,
        AIRDAP_DAP_PACKET_SIZE);
}

static size_t process(
    const uint8_t *request,
    size_t request_length,
    uint8_t *response)
{
    return process_owner(
        AIRDAP_DAP_OWNER_USB,
        request,
        request_length,
        response);
}

static void assert_line_reset(unsigned expected_calls)
{
    static const uint8_t expected[8] = {
        0xFFU, 0xFFU, 0xFFU, 0xFFU,
        0xFFU, 0xFFU, 0xFFU, 0xFFU,
    };
    assert(line_reset_calls == expected_calls);
    assert(last_line_reset_bits == 64U);
    assert(memcmp(last_line_reset, expected, sizeof(expected)) == 0);
}

int main(void)
{
    uint8_t response[AIRDAP_DAP_PACKET_SIZE];
    const uint8_t connect[] = {0x02U, AIRDAP_DAP_PORT_SWD};
    const uint8_t disconnect[] = {0x03U};
    const uint8_t ota_begin[] = {0x81U, 32U, 0U, 0U, 0U};
    airdap_dap_ownership_claim_t network_claim = {0};
    airdap_dap_ownership_claim_t wrong_claim = {
        .owner = AIRDAP_DAP_OWNER_NETWORK,
        .generation = 1U,
    };

    assert(airdap_dap_init("ADP-TEST", "test-version") == ESP_OK);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_NONE);

    assert(process(ota_begin, sizeof(ota_begin), response) == 2U);
    assert(ota_receiving);
    airdap_dap_session_closed(AIRDAP_DAP_OWNER_NETWORK);
    assert(ota_receiving && ota_disconnect_calls == 0U);
    airdap_dap_session_closed(AIRDAP_DAP_OWNER_USB);
    assert(!ota_receiving);
    assert(ota_disconnect_calls == 1U);
    assert(release_io_calls == 0U && release_reset_calls == 0U);

    assert(process(connect, sizeof(connect), response) == 2U);
    assert(response[1] == AIRDAP_DAP_PORT_SWD);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_USB);
    assert_line_reset(1U);

    assert(process_owner(
        AIRDAP_DAP_OWNER_NETWORK,
        connect,
        sizeof(connect),
        response) == 2U);
    assert(response[1] == AIRDAP_DAP_PORT_DISABLED);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_USB);

    const unsigned release_io_before_sequence = release_io_calls;
    const unsigned release_reset_before_sequence = release_reset_calls;
    const uint8_t swj_sequence[] = {0x12U, 8U, 0xA5U};
    revoke_during_sequence = true;
    assert(process(swj_sequence, sizeof(swj_sequence), response) == 2U);
    revoke_during_sequence = false;
    assert(response[1] == 0U);
    assert(sequence_revoke_result == AIRDAP_DAP_OWNERSHIP_BUSY);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_USB);
    assert(release_io_calls == release_io_before_sequence);
    assert(release_reset_calls == release_reset_before_sequence);

    assert(process(connect, sizeof(connect), response) == 2U);
    assert(response[1] == AIRDAP_DAP_PORT_SWD);
    assert_line_reset(1U);
    assert(airdap_dap_ownership_release(&wrong_claim) ==
        AIRDAP_DAP_OWNERSHIP_NOT_OWNER);

    assert(process(disconnect, sizeof(disconnect), response) == 2U);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_NONE);
    assert(release_io_calls == 1U && release_reset_calls == 1U);
    assert(ota_disconnect_calls == 2U);

    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_NETWORK,
        &network_claim) ==
        AIRDAP_DAP_OWNERSHIP_OK);
    assert_line_reset(2U);
    const unsigned target_reset_calls_before = target_reset_calls;
    const uint8_t swj_pins[] = {0x10U, 0x00U, 0x80U, 0U, 0U, 0U, 0U};
    assert(process(swj_pins, sizeof(swj_pins), response) == 2U);
    assert(response[1] == 0U);
    assert(target_reset_calls == target_reset_calls_before);
    assert(process(connect, sizeof(connect), response) == 2U);
    assert(response[1] == AIRDAP_DAP_PORT_DISABLED);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_NETWORK);
    assert(release_io_calls == 1U && release_reset_calls == 1U);
    assert(ota_disconnect_calls == 3U);

    assert(airdap_dap_ownership_release(&network_claim) ==
        AIRDAP_DAP_OWNERSHIP_OK);
    assert(release_io_calls == 2U && release_reset_calls == 2U);

    ota_debug_allowed = false;
    assert(process(connect, sizeof(connect), response) == 2U);
    assert(response[1] == AIRDAP_DAP_PORT_DISABLED);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_NONE);
    assert_line_reset(2U);

    ota_debug_allowed = true;
    line_reset_result = ESP_FAIL;
    assert(process(connect, sizeof(connect), response) == 2U);
    assert(response[1] == AIRDAP_DAP_PORT_DISABLED);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_NONE);
    assert_line_reset(3U);
    assert(release_io_calls == 3U && release_reset_calls == 3U);

    puts("DAP backend ownership tests passed");
    return 0;
}
