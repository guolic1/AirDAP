#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "airdap_debug_shell_swd_probe.h"

enum {
    FAKE_SET_CLOCK_ERROR = 101,
    FAKE_CONNECT_ERROR = 102,
    FAKE_RELEASE_ERROR = 103,
};

typedef struct {
    uint32_t clock_hz;
    uint64_t response_bits;
    int set_clock_error;
    int write_error;
    int read_error;
    int release_error;
    unsigned int set_clock_calls;
    unsigned int write_calls;
    unsigned int read_calls;
    unsigned int release_calls;
    size_t write_bit_count;
    size_t read_bit_count;
    uint8_t written_data[19];
} fake_swd_t;

static int fake_set_clock(void *context, uint32_t clock_hz)
{
    fake_swd_t *fake = context;
    ++fake->set_clock_calls;
    fake->clock_hz = clock_hz;
    return fake->set_clock_error;
}

static int fake_write_sequence(
    void *context,
    const uint8_t *data,
    size_t bit_count)
{
    fake_swd_t *fake = context;
    ++fake->write_calls;
    fake->write_bit_count = bit_count;
    if (bit_count == sizeof(fake->written_data) * 8U) {
        for (size_t index = 0U; index < sizeof(fake->written_data); ++index) {
            fake->written_data[index] = data[index];
        }
    }
    return fake->write_error;
}

static int fake_read_sequence(void *context, uint8_t *data, size_t bit_count)
{
    fake_swd_t *fake = context;
    ++fake->read_calls;
    fake->read_bit_count = bit_count;
    for (size_t index = 0U; index < 5U; ++index) {
        data[index] = (uint8_t) (fake->response_bits >> (index * 8U));
    }
    return fake->read_error;
}

static int fake_release(void *context)
{
    fake_swd_t *fake = context;
    ++fake->release_calls;
    return fake->release_error;
}

static airdap_debug_shell_swd_backend_t make_backend(fake_swd_t *fake)
{
    return (airdap_debug_shell_swd_backend_t) {
        .context = fake,
        .set_clock = fake_set_clock,
        .write_sequence = fake_write_sequence,
        .read_sequence = fake_read_sequence,
        .release = fake_release,
    };
}

static uint64_t make_response(uint8_t ack, uint32_t idcode, uint8_t parity)
{
    return (uint64_t) ack |
        ((uint64_t) idcode << 3U) |
        ((uint64_t) parity << 35U) |
        (UINT64_C(1) << 36U);
}

static void test_default_clock_reads_idcode_and_releases_bus(void)
{
    fake_swd_t fake = {
        .response_bits = make_response(1U, UINT32_C(0x2BA01477), 0U),
    };
    const airdap_debug_shell_swd_backend_t backend = make_backend(&fake);
    airdap_debug_shell_swd_probe_result_t result;

    assert(airdap_debug_shell_swd_probe("", &backend, &result) ==
        AIRDAP_DEBUG_SHELL_SWD_PROBE_OK);
    assert(fake.clock_hz == UINT32_C(100000));
    assert(fake.set_clock_calls == 1U);
    assert(fake.write_calls == 1U);
    assert(fake.read_calls == 1U);
    assert(fake.release_calls == 1U);
    assert(fake.write_bit_count == 152U);
    assert(fake.read_bit_count == 37U);
    for (size_t index = 0U; index < 8U; ++index) {
        assert(fake.written_data[index] == 0xFFU);
    }
    assert(fake.written_data[8] == 0x9EU);
    assert(fake.written_data[9] == 0xE7U);
    for (size_t index = 10U; index < 17U; ++index) {
        assert(fake.written_data[index] == 0xFFU);
    }
    assert(fake.written_data[17] == 0x00U);
    assert(fake.written_data[18] == 0xA5U);
    assert(result.clock_khz == 100U);
    assert(result.idcode == UINT32_C(0x2BA01477));
    assert(result.ack == 1U);
    assert(result.received_parity == 0U);
    assert(result.expected_parity == 0U);
    assert(result.response_bits == fake.response_bits);
    assert(result.operation_error == 0);
    assert(result.release_error == 0);
}

static void test_stm32f1_hardware_capture_starts_with_ack(void)
{
    fake_swd_t fake = {
        .response_bits = UINT64_C(0x30DD00A3B9),
    };
    const airdap_debug_shell_swd_backend_t backend = make_backend(&fake);
    airdap_debug_shell_swd_probe_result_t result;

    assert(airdap_debug_shell_swd_probe("500", &backend, &result) ==
        AIRDAP_DEBUG_SHELL_SWD_PROBE_OK);
    assert(result.ack == 1U);
    assert(result.idcode == UINT32_C(0x1BA01477));
    assert(result.received_parity == 0U);
    assert(result.expected_parity == 0U);
}

static void test_explicit_clock_is_parsed(void)
{
    fake_swd_t fake = {
        .response_bits = make_response(1U, UINT32_C(0x0BC11477), 0U),
    };
    const airdap_debug_shell_swd_backend_t backend = make_backend(&fake);
    airdap_debug_shell_swd_probe_result_t result;

    assert(airdap_debug_shell_swd_probe(" 500 ", &backend, &result) ==
        AIRDAP_DEBUG_SHELL_SWD_PROBE_OK);
    assert(fake.clock_hz == UINT32_C(500000));
    assert(result.clock_khz == 500U);
}

static void assert_input_rejected(
    const char *arguments,
    airdap_debug_shell_swd_probe_status_t expected_status)
{
    fake_swd_t fake = {0};
    const airdap_debug_shell_swd_backend_t backend = make_backend(&fake);
    airdap_debug_shell_swd_probe_result_t result;

    assert(airdap_debug_shell_swd_probe(arguments, &backend, &result) ==
        expected_status);
    assert(fake.set_clock_calls == 0U);
    assert(fake.write_calls == 0U);
    assert(fake.read_calls == 0U);
    assert(fake.release_calls == 0U);
}

static void test_invalid_input_does_not_touch_bus(void)
{
    assert_input_rejected("abc", AIRDAP_DEBUG_SHELL_SWD_PROBE_USAGE);
    assert_input_rejected("100 extra", AIRDAP_DEBUG_SHELL_SWD_PROBE_USAGE);
    assert_input_rejected("99", AIRDAP_DEBUG_SHELL_SWD_PROBE_CLOCK_OUT_OF_RANGE);
    assert_input_rejected("10001", AIRDAP_DEBUG_SHELL_SWD_PROBE_CLOCK_OUT_OF_RANGE);
    assert_input_rejected("42949672960", AIRDAP_DEBUG_SHELL_SWD_PROBE_CLOCK_OUT_OF_RANGE);
}

static void test_set_clock_failure_is_reported_and_bus_is_released(void)
{
    fake_swd_t fake = {.set_clock_error = FAKE_SET_CLOCK_ERROR};
    const airdap_debug_shell_swd_backend_t backend = make_backend(&fake);
    airdap_debug_shell_swd_probe_result_t result;

    assert(airdap_debug_shell_swd_probe("100", &backend, &result) ==
        AIRDAP_DEBUG_SHELL_SWD_PROBE_SET_CLOCK_FAILED);
    assert(fake.write_calls == 0U);
    assert(fake.read_calls == 0U);
    assert(fake.release_calls == 1U);
    assert(result.operation_error == FAKE_SET_CLOCK_ERROR);
}

static void test_sequence_write_failure_is_reported_and_bus_is_released(void)
{
    fake_swd_t fake = {.write_error = FAKE_CONNECT_ERROR};
    const airdap_debug_shell_swd_backend_t backend = make_backend(&fake);
    airdap_debug_shell_swd_probe_result_t result;

    assert(airdap_debug_shell_swd_probe("100", &backend, &result) ==
        AIRDAP_DEBUG_SHELL_SWD_PROBE_CONNECT_FAILED);
    assert(fake.write_calls == 1U);
    assert(fake.read_calls == 0U);
    assert(fake.release_calls == 1U);
    assert(result.operation_error == FAKE_CONNECT_ERROR);
}

static void test_sequence_read_failure_is_reported_and_bus_is_released(void)
{
    fake_swd_t fake = {.read_error = FAKE_CONNECT_ERROR};
    const airdap_debug_shell_swd_backend_t backend = make_backend(&fake);
    airdap_debug_shell_swd_probe_result_t result;

    assert(airdap_debug_shell_swd_probe("100", &backend, &result) ==
        AIRDAP_DEBUG_SHELL_SWD_PROBE_CONNECT_FAILED);
    assert(fake.write_calls == 1U);
    assert(fake.read_calls == 1U);
    assert(fake.release_calls == 1U);
    assert(result.operation_error == FAKE_CONNECT_ERROR);
}

static void test_raw_invalid_ack_is_reported(void)
{
    fake_swd_t fake = {
        .response_bits = make_response(7U, UINT32_C(0xFFFFFFFF), 1U),
    };
    const airdap_debug_shell_swd_backend_t backend = make_backend(&fake);
    airdap_debug_shell_swd_probe_result_t result;

    assert(airdap_debug_shell_swd_probe("100", &backend, &result) ==
        AIRDAP_DEBUG_SHELL_SWD_PROBE_RESPONSE_INVALID);
    assert(result.ack == 7U);
    assert(result.response_bits == fake.response_bits);
    assert(fake.release_calls == 1U);
}

static void test_bad_idcode_parity_is_reported(void)
{
    fake_swd_t fake = {
        .response_bits = make_response(1U, UINT32_C(0x1BA01477), 1U),
    };
    const airdap_debug_shell_swd_backend_t backend = make_backend(&fake);
    airdap_debug_shell_swd_probe_result_t result;

    assert(airdap_debug_shell_swd_probe("100", &backend, &result) ==
        AIRDAP_DEBUG_SHELL_SWD_PROBE_PARITY_ERROR);
    assert(result.idcode == UINT32_C(0x1BA01477));
    assert(result.received_parity == 1U);
    assert(result.expected_parity == 0U);
    assert(fake.release_calls == 1U);
}

static void test_release_failure_is_not_hidden(void)
{
    fake_swd_t fake = {
        .response_bits = make_response(1U, UINT32_C(0x2BA01477), 0U),
        .release_error = FAKE_RELEASE_ERROR,
    };
    const airdap_debug_shell_swd_backend_t backend = make_backend(&fake);
    airdap_debug_shell_swd_probe_result_t result;

    assert(airdap_debug_shell_swd_probe("100", &backend, &result) ==
        AIRDAP_DEBUG_SHELL_SWD_PROBE_RELEASE_FAILED);
    assert(result.idcode == UINT32_C(0x2BA01477));
    assert(result.release_error == FAKE_RELEASE_ERROR);
}

int main(void)
{
    test_default_clock_reads_idcode_and_releases_bus();
    test_stm32f1_hardware_capture_starts_with_ack();
    test_explicit_clock_is_parsed();
    test_invalid_input_does_not_touch_bus();
    test_set_clock_failure_is_reported_and_bus_is_released();
    test_sequence_write_failure_is_reported_and_bus_is_released();
    test_sequence_read_failure_is_reported_and_bus_is_released();
    test_raw_invalid_ack_is_reported();
    test_bad_idcode_parity_is_reported();
    test_release_failure_is_not_hidden();
    return 0;
}
