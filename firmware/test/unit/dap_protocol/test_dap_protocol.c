#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_dap_protocol.h"

enum {
    DAP_OK = 0,
    DAP_ERROR = 0xFF,
    DAP_TRANSFER_OK = 1,
};

typedef struct {
    bool connected;
    bool reset;
    uint32_t clock_hz;
    uint32_t transfer_data;
    uint32_t delay_us;
    uint32_t sequence_bits;
    uint32_t transfer_calls;
    uint16_t wait_retries;
    uint8_t idle_cycles;
    uint8_t turnaround_cycles;
    uint8_t last_address;
    uint8_t pin_value;
    uint8_t pin_select;
    bool data_phase;
    bool last_ap;
    bool last_read;
    bool fail_read_sequence;
    bool vendor_debug_connected;
    uint8_t vendor_command;
    unsigned vendor_calls;
} fake_backend_t;

static bool fake_connect(void *context)
{
    fake_backend_t *fake = context;
    fake->connected = true;
    return true;
}

static void fake_disconnect(void *context)
{
    fake_backend_t *fake = context;
    fake->connected = false;
}

static bool fake_set_clock(void *context, uint32_t clock_hz)
{
    fake_backend_t *fake = context;
    fake->clock_hz = clock_hz;
    return true;
}

static bool fake_configure_transfer(
    void *context,
    uint8_t idle_cycles,
    uint16_t wait_retries)
{
    fake_backend_t *fake = context;
    fake->idle_cycles = idle_cycles;
    fake->wait_retries = wait_retries;
    return true;
}

static bool fake_configure_swd(
    void *context,
    uint8_t turnaround_cycles,
    bool data_phase)
{
    fake_backend_t *fake = context;
    fake->turnaround_cycles = turnaround_cycles;
    fake->data_phase = data_phase;
    return true;
}

static bool fake_transfer(
    void *context,
    bool ap,
    bool read,
    uint8_t address,
    uint32_t *data,
    uint8_t *status)
{
    fake_backend_t *fake = context;
    ++fake->transfer_calls;
    fake->last_ap = ap;
    fake->last_read = read;
    fake->last_address = address;
    if (read) {
        *data = fake->transfer_data++;
    } else {
        fake->transfer_data = *data;
    }
    *status = DAP_TRANSFER_OK;
    return true;
}

static bool fake_write_sequence(
    void *context,
    const uint8_t *data,
    size_t bit_count)
{
    fake_backend_t *fake = context;
    assert(data != NULL);
    fake->sequence_bits += bit_count;
    return true;
}

static bool fake_read_sequence(
    void *context,
    uint8_t *data,
    size_t bit_count)
{
    fake_backend_t *fake = context;
    fake->sequence_bits += bit_count;
    if (fake->fail_read_sequence) {
        return false;
    }
    memset(data, 0xA5, (bit_count + 7U) / 8U);
    return true;
}

static bool fake_swj_pins(
    void *context,
    uint8_t value,
    uint8_t select,
    uint32_t wait_us,
    uint8_t *pins)
{
    fake_backend_t *fake = context;
    fake->pin_value = value;
    fake->pin_select = select;
    fake->delay_us = wait_us;
    *pins = value & select;
    return true;
}

static bool fake_reset(void *context)
{
    fake_backend_t *fake = context;
    fake->reset = true;
    return true;
}

static void fake_delay(void *context, uint16_t delay_us)
{
    fake_backend_t *fake = context;
    fake->delay_us = delay_us;
}

static size_t fake_vendor_command(
    void *context,
    bool debug_connected,
    const uint8_t *request,
    size_t request_length,
    uint8_t *response,
    size_t response_capacity)
{
    fake_backend_t *fake = context;
    assert(request != NULL && request_length > 0U);
    assert(response != NULL && response_capacity >= 2U);
    fake->vendor_debug_connected = debug_connected;
    fake->vendor_command = request[0];
    ++fake->vendor_calls;
    response[0] = request[0];
    response[1] = 0x5AU;
    return 2U;
}

static airdap_dap_backend_t make_backend(fake_backend_t *fake)
{
    return (airdap_dap_backend_t) {
        .context = fake,
        .connect = fake_connect,
        .disconnect = fake_disconnect,
        .set_clock = fake_set_clock,
        .configure_transfer = fake_configure_transfer,
        .configure_swd = fake_configure_swd,
        .transfer = fake_transfer,
        .write_sequence = fake_write_sequence,
        .read_sequence = fake_read_sequence,
        .swj_pins = fake_swj_pins,
        .reset_target = fake_reset,
        .delay_us = fake_delay,
        .vendor_command = fake_vendor_command,
    };
}

static size_t process(
    airdap_dap_processor_t *processor,
    const uint8_t *request,
    size_t request_length,
    uint8_t *response)
{
    memset(response, 0xCC, AIRDAP_DAP_PACKET_SIZE);
    return airdap_dap_process_packet(
        processor,
        request,
        request_length,
        response,
        AIRDAP_DAP_PACKET_SIZE);
}

static void test_info(airdap_dap_processor_t *processor)
{
    uint8_t response[AIRDAP_DAP_PACKET_SIZE];
    const uint8_t serial_request[] = {0x00, 0x03};
    size_t length = process(
        processor, serial_request, sizeof(serial_request), response);
    assert(length == 2U + sizeof("ADP-1234"));
    assert(response[0] == 0x00 && response[1] == sizeof("ADP-1234"));
    assert(strcmp((char *) response + 2U, "ADP-1234") == 0);

    const uint8_t firmware_version_request[] = {0x00, 0x09};
    length = process(
        processor,
        firmware_version_request,
        sizeof(firmware_version_request),
        response);
    assert(length == 2U + sizeof("v1.2.3-test"));
    assert(response[0] == 0x00 && response[1] == sizeof("v1.2.3-test"));
    assert(strcmp((char *) response + 2U, "v1.2.3-test") == 0);

    const uint8_t packet_size_request[] = {0x00, 0xFF};
    length = process(
        processor, packet_size_request, sizeof(packet_size_request), response);
    assert(length == 4U);
    assert(response[1] == 2U);
    assert(response[2] == (uint8_t) AIRDAP_DAP_PACKET_SIZE);
    assert(response[3] == (uint8_t) (AIRDAP_DAP_PACKET_SIZE >> 8U));
    assert((AIRDAP_DAP_PACKET_SIZE % 64U) != 0U);

    const uint8_t capabilities_request[] = {0x00, 0xF0};
    length = process(
        processor, capabilities_request, sizeof(capabilities_request), response);
    assert(length == 4U);
    assert(response[2] == 0x01 && response[3] == 0x01);
}

static void test_control_commands(
    airdap_dap_processor_t *processor,
    fake_backend_t *fake)
{
    uint8_t response[AIRDAP_DAP_PACKET_SIZE];
    const uint8_t connect[] = {0x02, 0x00};
    assert(process(processor, connect, sizeof(connect), response) == 2U);
    assert(response[1] == AIRDAP_DAP_PORT_SWD && fake->connected);

    const uint8_t configure[] = {0x04, 7, 0x34, 0x12, 0x78, 0x56};
    assert(process(processor, configure, sizeof(configure), response) == 2U);
    assert(response[1] == DAP_OK);
    assert(fake->idle_cycles == 7U && fake->wait_retries == 0x1234U);
    assert(processor->match_retries == 0x5678U);

    const uint8_t clock[] = {0x11, 0x40, 0x42, 0x0F, 0x00};
    assert(process(processor, clock, sizeof(clock), response) == 2U);
    assert(fake->clock_hz == 1000000U);

    const uint8_t swd_config[] = {0x13, 0x06};
    assert(process(processor, swd_config, sizeof(swd_config), response) == 2U);
    assert(fake->turnaround_cycles == 3U && fake->data_phase);

    const uint8_t swj_pins[] = {0x10, 0x82, 0x82, 1, 0, 0, 0};
    assert(process(processor, swj_pins, sizeof(swj_pins), response) == 2U);
    assert(response[1] == 0x82 && fake->delay_us == 1U);

    const uint8_t delay[] = {0x09, 0x34, 0x12};
    assert(process(processor, delay, sizeof(delay), response) == 2U);
    assert(fake->delay_us == 0x1234U);

    const uint8_t reset[] = {0x0A};
    assert(process(processor, reset, sizeof(reset), response) == 3U);
    assert(response[1] == DAP_OK && response[2] == 0U && fake->reset);
}

static void test_swj_pins_are_available_without_connect(void)
{
    fake_backend_t fake = {0};
    const airdap_dap_backend_t backend = make_backend(&fake);
    airdap_dap_processor_t processor;
    uint8_t response[AIRDAP_DAP_PACKET_SIZE];
    const uint8_t swj_pins[] = {0x10, 0x80, 0x80, 0, 0, 0, 0};

    airdap_dap_processor_init(
        &processor,
        &backend,
        "ADP-UNCONNECTED",
        "test-version");
    assert(process(&processor, swj_pins, sizeof(swj_pins), response) == 2U);
    assert(response[0] == 0x10 && response[1] == 0x80);
    assert(fake.pin_value == 0x80 && fake.pin_select == 0x80);
}

static void test_unsupported_connect_releases_backend(void)
{
    fake_backend_t fake = {.connected = true};
    const airdap_dap_backend_t backend = make_backend(&fake);
    airdap_dap_processor_t processor;
    uint8_t response[AIRDAP_DAP_PACKET_SIZE];
    const uint8_t unsupported_connect[] = {0x02, 0x02};

    airdap_dap_processor_init(
        &processor,
        &backend,
        "ADP-UNSUPPORTED",
        "test-version");
    assert(process(
        &processor,
        unsupported_connect,
        sizeof(unsupported_connect),
        response) == 2U);
    assert(response[0] == 0x02 && response[1] == AIRDAP_DAP_PORT_DISABLED);
    assert(!fake.connected);
}

static void test_sequences(
    airdap_dap_processor_t *processor,
    fake_backend_t *fake)
{
    uint8_t response[AIRDAP_DAP_PACKET_SIZE];
    const uint8_t swj[] = {0x12, 9, 0x9E, 0x01};
    assert(process(processor, swj, sizeof(swj), response) == 2U);
    assert(response[1] == DAP_OK && fake->sequence_bits == 9U);

    const uint8_t swd[] = {
        0x1D, 2,
        8, 0x5A,
        0x80 | 8,
    };
    assert(process(processor, swd, sizeof(swd), response) == 3U);
    assert(response[1] == DAP_OK && response[2] == 0xA5);
    assert(fake->sequence_bits == 25U);

    fake->fail_read_sequence = true;
    const uint8_t failed_read[] = {0x1D, 1, 0x80 | 8};
    assert(process(processor, failed_read, sizeof(failed_read), response) == 3U);
    assert(response[0] == 0x1D && response[1] == DAP_ERROR);
    assert(response[2] == 0U);
    fake->fail_read_sequence = false;
}

static void test_transfers(
    airdap_dap_processor_t *processor,
    fake_backend_t *fake)
{
    uint8_t response[AIRDAP_DAP_PACKET_SIZE];
    fake->transfer_data = 0x11223344U;
    const uint8_t request[] = {
        0x05, 0, 2,
        0x0F,
        0x00, 0xDD, 0xCC, 0xBB, 0xAA,
    };
    assert(process(processor, request, sizeof(request), response) == 7U);
    assert(response[0] == 0x05 && response[1] == 2 && response[2] == DAP_TRANSFER_OK);
    assert(response[3] == 0x44 && response[4] == 0x33 &&
        response[5] == 0x22 && response[6] == 0x11);
    assert(!fake->last_ap && fake->last_read && fake->last_address == 0x0CU);
    assert(fake->transfer_data == 0xAABBCCDEU);

    fake->transfer_data = 0x01020304U;
    const uint8_t block[] = {0x06, 0, 2, 0, 0x03};
    assert(process(processor, block, sizeof(block), response) == 12U);
    assert(response[1] == 2 && response[2] == 0 && response[3] == DAP_TRANSFER_OK);
    assert(response[4] == 0x04 && response[8] == 0x05);

    const uint8_t maximum_block[] = {0x06, 0, 126, 0, 0x03};
    assert(process(processor, maximum_block, sizeof(maximum_block), response) ==
        AIRDAP_DAP_PACKET_SIZE);
    assert(response[1] == 126U && response[2] == 0U &&
        response[3] == DAP_TRANSFER_OK);

    uint8_t oversized_response[AIRDAP_DAP_BUFFER_SIZE];
    const uint8_t oversized_block[] = {0x06, 0, 127, 0, 0x03};
    assert(airdap_dap_process_packet(
        processor,
        oversized_block,
        sizeof(oversized_block),
        oversized_response,
        sizeof(oversized_response)) == 1U);
    assert(oversized_response[0] == DAP_ERROR);

    const uint8_t abort[] = {0x08, 0, 0x1E, 0, 0, 0};
    assert(process(processor, abort, sizeof(abort), response) == 2U);
    assert(response[1] == DAP_OK && fake->transfer_data == 0x1EU);
}

static void test_disconnect_and_invalid(
    airdap_dap_processor_t *processor,
    fake_backend_t *fake)
{
    uint8_t response[AIRDAP_DAP_PACKET_SIZE];
    const uint8_t disconnect[] = {0x03};
    assert(process(processor, disconnect, sizeof(disconnect), response) == 2U);
    assert(response[1] == DAP_OK && !fake->connected);

    const uint8_t unknown[] = {0x55};
    assert(process(processor, unknown, sizeof(unknown), response) == 1U);
    assert(response[0] == DAP_ERROR);

    const uint8_t short_transfer[] = {0x05, 0};
    assert(process(processor, short_transfer, sizeof(short_transfer), response) == 1U);
    assert(response[0] == DAP_ERROR);
}

static void test_request_framing(void)
{
    const uint8_t info[] = {0x00, 0xFF};
    assert(airdap_dap_request_size(info, 1U) == 0U);
    assert(airdap_dap_request_size(info, sizeof(info)) == sizeof(info));

    const uint8_t transfer[] = {
        0x05, 0, 2,
        0x02,
        0x00, 0x78, 0x56, 0x34, 0x12,
    };
    assert(airdap_dap_request_size(transfer, 6U) == 0U);
    assert(airdap_dap_request_size(transfer, sizeof(transfer)) == sizeof(transfer));

    const uint8_t block_write[] = {
        0x06, 0, 2, 0, 0,
        1, 2, 3, 4,
        5, 6, 7, 8,
    };
    assert(airdap_dap_request_size(block_write, 12U) == 0U);
    assert(airdap_dap_request_size(block_write, sizeof(block_write)) ==
        sizeof(block_write));

    const uint8_t swd_sequence[] = {
        0x1D, 2,
        8, 0xA5,
        0x80 | 8,
    };
    assert(airdap_dap_request_size(swd_sequence, 4U) == 0U);
    assert(airdap_dap_request_size(swd_sequence, sizeof(swd_sequence)) ==
        sizeof(swd_sequence));

    const uint8_t ota_query[] = {0x80};
    assert(airdap_dap_request_size(ota_query, sizeof(ota_query)) == 1U);

    const uint8_t ota_begin[] = {0x81, 1, 0, 0, 0};
    assert(airdap_dap_request_size(ota_begin, 4U) == 0U);
    assert(airdap_dap_request_size(ota_begin, sizeof(ota_begin)) ==
        sizeof(ota_begin));

    const uint8_t ota_write[] = {0x82, 0, 0, 0, 0, 3, 0, 1, 2, 3};
    assert(airdap_dap_request_size(ota_write, 9U) == 0U);
    assert(airdap_dap_request_size(ota_write, sizeof(ota_write)) ==
        sizeof(ota_write));

    uint8_t oversized_ota_write[AIRDAP_DAP_PACKET_SIZE] = {0};
    oversized_ota_write[0] = 0x82;
    oversized_ota_write[5] = 497U & 0xFFU;
    oversized_ota_write[6] = 497U >> 8U;
    assert(airdap_dap_request_size(
        oversized_ota_write,
        sizeof(oversized_ota_write)) == SIZE_MAX);

    for (uint8_t command = 0x83U; command <= 0x85U; ++command) {
        const uint8_t request[] = {command};
        assert(airdap_dap_request_size(request, sizeof(request)) == 1U);
    }
}

static void test_vendor_commands(
    airdap_dap_processor_t *processor,
    fake_backend_t *fake)
{
    uint8_t response[AIRDAP_DAP_PACKET_SIZE];
    const uint8_t query[] = {0x80};

    const unsigned calls_before = fake->vendor_calls;
    assert(process(processor, query, sizeof(query), response) == 2U);
    assert(response[0] == 0x80U && response[1] == 0x5AU);
    assert(fake->vendor_calls == calls_before + 1U);
    assert(fake->vendor_command == 0x80U);
    assert(!fake->vendor_debug_connected);

    const uint8_t connect[] = {0x02, 0x01};
    assert(process(processor, connect, sizeof(connect), response) == 2U);
    assert(process(processor, query, sizeof(query), response) == 2U);
    assert(fake->vendor_debug_connected);

    const uint8_t disconnect[] = {0x03};
    assert(process(processor, disconnect, sizeof(disconnect), response) == 2U);
}

int main(void)
{
    fake_backend_t fake = {0};
    const airdap_dap_backend_t backend = make_backend(&fake);
    airdap_dap_processor_t processor;
    airdap_dap_processor_init(
        &processor,
        &backend,
        "ADP-1234",
        "v1.2.3-test");

    test_info(&processor);
    test_swj_pins_are_available_without_connect();
    test_unsupported_connect_releases_backend();
    test_control_commands(&processor, &fake);
    test_sequences(&processor, &fake);
    test_transfers(&processor, &fake);
    test_disconnect_and_invalid(&processor, &fake);
    test_request_framing();
    test_vendor_commands(&processor, &fake);

    puts("DAP protocol tests passed");
    return 0;
}
