#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "airdap_swd_protocol.h"

enum {
    EVENT_CAPACITY = 96,
    READ_CAPACITY = 16,
};

typedef enum {
    EVENT_SET_HOST_OUTPUT,
    EVENT_WRITE_BITS,
    EVENT_READ_BITS,
    EVENT_TURNAROUND,
} event_type_t;

typedef struct {
    event_type_t type;
    uint64_t bits;
    size_t bit_count;
    bool host_output;
} event_t;

typedef struct {
    uint64_t bits;
    size_t bit_count;
} queued_read_t;

typedef struct {
    event_t events[EVENT_CAPACITY];
    size_t event_count;
    queued_read_t reads[READ_CAPACITY];
    size_t read_count;
    size_t next_read;
} fake_io_t;

static void record_event(fake_io_t *fake, event_t event)
{
    assert(fake->event_count < EVENT_CAPACITY);
    fake->events[fake->event_count++] = event;
}

static esp_err_t fake_set_host_output(void *context, bool enabled)
{
    fake_io_t *fake = context;
    record_event(fake, (event_t) {
        .type = EVENT_SET_HOST_OUTPUT,
        .host_output = enabled,
    });
    return ESP_OK;
}

static esp_err_t fake_write_bits(void *context, uint64_t bits, size_t bit_count)
{
    fake_io_t *fake = context;
    record_event(fake, (event_t) {
        .type = EVENT_WRITE_BITS,
        .bits = bits,
        .bit_count = bit_count,
    });
    return ESP_OK;
}

static esp_err_t fake_read_bits(void *context, size_t bit_count, uint64_t *bits)
{
    fake_io_t *fake = context;
    assert(bits != NULL);
    assert(fake->next_read < fake->read_count);
    assert(fake->reads[fake->next_read].bit_count == bit_count);

    *bits = fake->reads[fake->next_read++].bits;
    record_event(fake, (event_t) {
        .type = EVENT_READ_BITS,
        .bits = *bits,
        .bit_count = bit_count,
    });
    return ESP_OK;
}

static esp_err_t fake_turnaround(void *context, bool host_output)
{
    fake_io_t *fake = context;
    record_event(fake, (event_t) {
        .type = EVENT_TURNAROUND,
        .host_output = host_output,
    });
    return ESP_OK;
}

static airdap_swd_io_t make_io(fake_io_t *fake)
{
    return (airdap_swd_io_t) {
        .context = fake,
        .idle_cycles = 8,
        .data_phase = false,
        .set_host_output = fake_set_host_output,
        .write_bits = fake_write_bits,
        .read_bits = fake_read_bits,
        .turnaround = fake_turnaround,
    };
}

static void queue_read(fake_io_t *fake, uint64_t bits, size_t bit_count)
{
    assert(fake->read_count < READ_CAPACITY);
    fake->reads[fake->read_count++] = (queued_read_t) {
        .bits = bits,
        .bit_count = bit_count,
    };
}

static unsigned int parity32(uint32_t value)
{
    unsigned int parity = 0U;
    while (value != 0U) {
        parity ^= value & 1U;
        value >>= 1U;
    }
    return parity;
}

static uint64_t data_with_parity(uint32_t value)
{
    return (uint64_t) value | ((uint64_t) parity32(value) << 32U);
}

static void assert_event(
    const fake_io_t *fake,
    size_t index,
    event_type_t type,
    uint64_t bits,
    size_t bit_count)
{
    assert(index < fake->event_count);
    assert(fake->events[index].type == type);
    assert(fake->events[index].bits == bits);
    assert(fake->events[index].bit_count == bit_count);
}

static void test_request_encoding(void)
{
    assert(airdap_swd_encode_request(&(airdap_swd_request_t) {
        .port = AIRDAP_SWD_PORT_DP,
        .direction = AIRDAP_SWD_READ,
        .address = 0x0,
    }) == 0xA5U);
    assert(airdap_swd_encode_request(&(airdap_swd_request_t) {
        .port = AIRDAP_SWD_PORT_AP,
        .direction = AIRDAP_SWD_READ,
        .address = 0xC,
    }) == 0x9FU);
    assert(airdap_swd_encode_request(&(airdap_swd_request_t) {
        .port = AIRDAP_SWD_PORT_DP,
        .direction = AIRDAP_SWD_WRITE,
        .address = 0x8,
    }) == 0xB1U);
}

static void test_dp_read_and_parity(void)
{
    const uint32_t idcode = UINT32_C(0x2BA01477);
    fake_io_t fake = {0};
    airdap_swd_io_t io = make_io(&fake);
    uint32_t data = 0U;
    airdap_swd_ack_t ack = AIRDAP_SWD_ACK_NONE;

    queue_read(&fake, AIRDAP_SWD_ACK_OK, 3U);
    queue_read(&fake, data_with_parity(idcode), 33U);

    assert(airdap_swd_protocol_transfer(
        &io,
        &(airdap_swd_request_t) {
            .port = AIRDAP_SWD_PORT_DP,
            .direction = AIRDAP_SWD_READ,
            .address = 0x0,
        },
        &data,
        0U,
        &ack) == ESP_OK);
    assert(ack == AIRDAP_SWD_ACK_OK);
    assert(data == idcode);
    assert(fake.event_count == 6U);
    assert_event(&fake, 0U, EVENT_WRITE_BITS, 0xA5U, 8U);
    assert(fake.events[1].type == EVENT_TURNAROUND);
    assert(!fake.events[1].host_output);
    assert_event(&fake, 2U, EVENT_READ_BITS, AIRDAP_SWD_ACK_OK, 3U);
    assert_event(&fake, 3U, EVENT_READ_BITS, data_with_parity(idcode), 33U);
    assert(fake.events[4].type == EVENT_TURNAROUND);
    assert(fake.events[4].host_output);
    assert_event(&fake, 5U, EVENT_WRITE_BITS, 0U, 8U);
}

static void test_write_data_and_parity(void)
{
    const uint32_t select = UINT32_C(0xF0000001);
    fake_io_t fake = {0};
    airdap_swd_io_t io = make_io(&fake);
    uint32_t data = select;
    airdap_swd_ack_t ack = AIRDAP_SWD_ACK_NONE;

    queue_read(&fake, AIRDAP_SWD_ACK_OK, 3U);

    assert(airdap_swd_protocol_transfer(
        &io,
        &(airdap_swd_request_t) {
            .port = AIRDAP_SWD_PORT_DP,
            .direction = AIRDAP_SWD_WRITE,
            .address = 0x8,
        },
        &data,
        0U,
        &ack) == ESP_OK);
    assert(ack == AIRDAP_SWD_ACK_OK);
    assert_event(&fake, 4U, EVENT_WRITE_BITS, data_with_parity(select), 33U);
    assert_event(&fake, 5U, EVENT_WRITE_BITS, 0U, 8U);
}

static void test_wait_is_retried(void)
{
    fake_io_t fake = {0};
    airdap_swd_io_t io = make_io(&fake);
    uint32_t data = 0U;
    airdap_swd_ack_t ack = AIRDAP_SWD_ACK_NONE;

    queue_read(&fake, AIRDAP_SWD_ACK_WAIT, 3U);
    queue_read(&fake, AIRDAP_SWD_ACK_OK, 3U);
    queue_read(&fake, data_with_parity(UINT32_C(0x12345678)), 33U);

    assert(airdap_swd_protocol_transfer(
        &io,
        &(airdap_swd_request_t) {
            .port = AIRDAP_SWD_PORT_DP,
            .direction = AIRDAP_SWD_READ,
            .address = 0x0,
        },
        &data,
        1U,
        &ack) == ESP_OK);
    assert(ack == AIRDAP_SWD_ACK_OK);
    assert(data == UINT32_C(0x12345678));
    assert(fake.next_read == 3U);
}

static void test_long_idle_cycles_are_chunked(void)
{
    fake_io_t fake = {0};
    airdap_swd_io_t io = make_io(&fake);
    io.idle_cycles = 255U;
    uint32_t data = UINT32_C(0x12345678);
    airdap_swd_ack_t ack = AIRDAP_SWD_ACK_NONE;

    queue_read(&fake, AIRDAP_SWD_ACK_OK, 3U);
    assert(airdap_swd_protocol_transfer(
        &io,
        &(airdap_swd_request_t) {
            .port = AIRDAP_SWD_PORT_DP,
            .direction = AIRDAP_SWD_WRITE,
            .address = 0x0,
        },
        &data,
        0U,
        &ack) == ESP_OK);
    assert(ack == AIRDAP_SWD_ACK_OK);
    assert_event(&fake, fake.event_count - 4U, EVENT_WRITE_BITS, 0U, 64U);
    assert_event(&fake, fake.event_count - 3U, EVENT_WRITE_BITS, 0U, 64U);
    assert_event(&fake, fake.event_count - 2U, EVENT_WRITE_BITS, 0U, 64U);
    assert_event(&fake, fake.event_count - 1U, EVENT_WRITE_BITS, 0U, 63U);
}

static void test_wait_data_phase_is_clocked(void)
{
    fake_io_t fake = {0};
    airdap_swd_io_t io = make_io(&fake);
    io.data_phase = true;
    uint32_t data = 0U;
    airdap_swd_ack_t ack = AIRDAP_SWD_ACK_NONE;

    queue_read(&fake, AIRDAP_SWD_ACK_WAIT, 3U);
    queue_read(&fake, 0U, 33U);
    assert(airdap_swd_protocol_transfer(
        &io,
        &(airdap_swd_request_t) {
            .port = AIRDAP_SWD_PORT_DP,
            .direction = AIRDAP_SWD_READ,
            .address = 0x0,
        },
        &data,
        0U,
        &ack) == ESP_OK);
    assert(ack == AIRDAP_SWD_ACK_WAIT);
    assert_event(&fake, 3U, EVENT_READ_BITS, 0U, 33U);
}

static void test_bad_data_parity_is_rejected_after_bus_recovery(void)
{
    fake_io_t fake = {0};
    airdap_swd_io_t io = make_io(&fake);
    uint32_t data = 0U;
    airdap_swd_ack_t ack = AIRDAP_SWD_ACK_NONE;

    queue_read(&fake, AIRDAP_SWD_ACK_OK, 3U);
    queue_read(&fake, data_with_parity(UINT32_C(0xAAAAAAAA)) ^ (UINT64_C(1) << 32U), 33U);

    assert(airdap_swd_protocol_transfer(
        &io,
        &(airdap_swd_request_t) {
            .port = AIRDAP_SWD_PORT_DP,
            .direction = AIRDAP_SWD_READ,
            .address = 0x0,
        },
        &data,
        0U,
        &ack) == ESP_ERR_INVALID_CRC);
    assert(fake.events[fake.event_count - 2U].type == EVENT_TURNAROUND);
    assert(fake.events[fake.event_count - 1U].type == EVENT_WRITE_BITS);
}

static void test_invalid_ack_backs_off_full_data_phase(void)
{
    fake_io_t fake = {0};
    airdap_swd_io_t io = make_io(&fake);
    uint32_t data = 0U;
    airdap_swd_ack_t ack = AIRDAP_SWD_ACK_NONE;

    queue_read(&fake, 7U, 3U);
    queue_read(&fake, 0U, 33U);

    assert(airdap_swd_protocol_transfer(
        &io,
        &(airdap_swd_request_t) {
            .port = AIRDAP_SWD_PORT_DP,
            .direction = AIRDAP_SWD_READ,
            .address = 0x0,
        },
        &data,
        0U,
        &ack) == ESP_ERR_INVALID_RESPONSE);
    assert(ack == 7U);
    assert_event(&fake, 3U, EVENT_READ_BITS, 0U, 33U);
    assert(fake.events[4].type == EVENT_TURNAROUND);
    assert(fake.events[4].host_output);
    assert_event(&fake, 5U, EVENT_WRITE_BITS, 0U, 8U);
}

static void test_connect_sequence_reads_idcode(void)
{
    const uint32_t idcode = UINT32_C(0x2BA01477);
    fake_io_t fake = {0};
    airdap_swd_io_t io = make_io(&fake);
    uint32_t result = 0U;

    queue_read(&fake, AIRDAP_SWD_ACK_OK, 3U);
    queue_read(&fake, data_with_parity(idcode), 33U);

    assert(airdap_swd_protocol_connect(&io, &result) == ESP_OK);
    assert(result == idcode);
    assert(fake.events[0].type == EVENT_SET_HOST_OUTPUT);
    assert(fake.events[0].host_output);
    assert_event(&fake, 1U, EVENT_WRITE_BITS, UINT64_MAX, 64U);
    assert_event(&fake, 2U, EVENT_WRITE_BITS, UINT64_C(0xE79E), 16U);
    assert_event(
        &fake,
        3U,
        EVENT_WRITE_BITS,
        (UINT64_C(1) << 56U) - 1U,
        56U);
    assert_event(&fake, 4U, EVENT_WRITE_BITS, 0U, 8U);
    assert_event(&fake, 5U, EVENT_WRITE_BITS, 0xA5U, 8U);
}

int main(void)
{
    test_request_encoding();
    test_dp_read_and_parity();
    test_write_data_and_parity();
    test_wait_is_retried();
    test_long_idle_cycles_are_chunked();
    test_wait_data_phase_is_clocked();
    test_bad_data_parity_is_rejected_after_bus_recovery();
    test_invalid_ack_backs_off_full_data_phase();
    test_connect_sequence_reads_idcode();

    puts("SWD protocol tests passed");
    return 0;
}
