#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airdap_dap_ownership.h"
#include "airdap_debug_shell_swd_probe.h"

enum {
    FAKE_SET_CLOCK_ERROR = 101,
    FAKE_CONNECT_ERROR = 102,
};

typedef struct {
    uint32_t clock_hz;
    uint64_t response_bits;
    int set_clock_error;
    int write_error;
    int read_error;
    unsigned int set_clock_calls;
    unsigned int write_calls;
    unsigned int read_calls;
    unsigned int cancelled_calls;
    unsigned int cancel_on_check;
    size_t write_bit_count;
    size_t read_bit_count;
    uint8_t written_data[19];
    bool revoke_during_write;
    airdap_dap_ownership_result_t revoke_result;
} fake_swd_t;

typedef struct {
    unsigned int line_reset_calls;
    unsigned int release_pins_calls;
    bool release_pins_success;
} fake_ownership_backend_t;

typedef struct {
    unsigned int line_reset_calls;
    unsigned int release_pins_calls;
} ownership_counts_t;

static fake_ownership_backend_t ownership_fake;

static bool fake_ownership_line_reset(void *context)
{
    fake_ownership_backend_t *fake = context;
    ++fake->line_reset_calls;
    return true;
}

static bool fake_ownership_release_pins(void *context)
{
    fake_ownership_backend_t *fake = context;
    ++fake->release_pins_calls;
    return fake->release_pins_success;
}

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
    if (fake->revoke_during_write) {
        fake->revoke_result = airdap_dap_ownership_revoke();
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

static bool fake_cancelled(void *context)
{
    fake_swd_t *fake = context;
    ++fake->cancelled_calls;
    return fake->cancel_on_check != 0U &&
        fake->cancelled_calls >= fake->cancel_on_check;
}

static airdap_debug_shell_swd_backend_t make_backend(fake_swd_t *fake)
{
    return (airdap_debug_shell_swd_backend_t) {
        .context = fake,
        .set_clock = fake_set_clock,
        .write_sequence = fake_write_sequence,
        .read_sequence = fake_read_sequence,
        .cancelled = fake_cancelled,
    };
}

static ownership_counts_t ownership_counts(void)
{
    return (ownership_counts_t) {
        .line_reset_calls = ownership_fake.line_reset_calls,
        .release_pins_calls = ownership_fake.release_pins_calls,
    };
}

static void assert_diagnostic_owner_released(ownership_counts_t before)
{
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_NONE);
    assert(ownership_fake.line_reset_calls == before.line_reset_calls + 1U);
    assert(ownership_fake.release_pins_calls ==
        before.release_pins_calls + 1U);
}

static uint64_t make_response(uint8_t ack, uint32_t idcode, uint8_t parity)
{
    return (uint64_t) ack |
        ((uint64_t) idcode << 3U) |
        ((uint64_t) parity << 35U) |
        (UINT64_C(1) << 36U);
}

static void test_existing_owners_block_probe_without_touching_swd(void)
{
    static const airdap_dap_owner_t owners[] = {
        AIRDAP_DAP_OWNER_USB,
        AIRDAP_DAP_OWNER_NETWORK,
        AIRDAP_DAP_OWNER_DIAGNOSTIC,
    };

    for (size_t index = 0U; index < sizeof(owners) / sizeof(owners[0]); ++index) {
        airdap_dap_ownership_claim_t claim = {0};
        assert(airdap_dap_ownership_acquire(owners[index], &claim) ==
            AIRDAP_DAP_OWNERSHIP_OK);
        fake_swd_t fake = {
            .response_bits = make_response(1U, UINT32_C(0x2BA01477), 0U),
        };
        const airdap_debug_shell_swd_backend_t backend = make_backend(&fake);
        airdap_debug_shell_swd_probe_result_t result;
        const ownership_counts_t before = ownership_counts();

        assert(airdap_debug_shell_swd_probe("100", &backend, &result) ==
            AIRDAP_DEBUG_SHELL_SWD_PROBE_BUSY);

        assert(fake.set_clock_calls == 0U);
        assert(fake.write_calls == 0U);
        assert(fake.read_calls == 0U);
        assert(airdap_dap_ownership_current() == owners[index]);
        assert(ownership_fake.line_reset_calls == before.line_reset_calls);
        assert(ownership_fake.release_pins_calls == before.release_pins_calls);
        assert(airdap_dap_ownership_release(&claim) ==
            AIRDAP_DAP_OWNERSHIP_OK);
    }
}

static void test_probe_blocks_revoke_and_releases_diagnostic_owner(void)
{
    const ownership_counts_t before = ownership_counts();
    fake_swd_t fake = {
        .response_bits = make_response(1U, UINT32_C(0x2BA01477), 0U),
        .revoke_during_write = true,
    };
    const airdap_debug_shell_swd_backend_t backend = make_backend(&fake);
    airdap_debug_shell_swd_probe_result_t result;

    assert(airdap_debug_shell_swd_probe("100", &backend, &result) ==
        AIRDAP_DEBUG_SHELL_SWD_PROBE_OK);
    assert(fake.revoke_result == AIRDAP_DAP_OWNERSHIP_BUSY);
    assert_diagnostic_owner_released(before);
}

static void test_default_clock_reads_idcode_and_releases_bus(void)
{
    fake_swd_t fake = {
        .response_bits = make_response(1U, UINT32_C(0x2BA01477), 0U),
    };
    const airdap_debug_shell_swd_backend_t backend = make_backend(&fake);
    airdap_debug_shell_swd_probe_result_t result;
    const ownership_counts_t before = ownership_counts();

    assert(airdap_debug_shell_swd_probe("", &backend, &result) ==
        AIRDAP_DEBUG_SHELL_SWD_PROBE_OK);
    assert(fake.clock_hz == UINT32_C(100000));
    assert(fake.set_clock_calls == 1U);
    assert(fake.write_calls == 1U);
    assert(fake.read_calls == 1U);
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
    assert_diagnostic_owner_released(before);
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
    const ownership_counts_t before = ownership_counts();

    assert(airdap_debug_shell_swd_probe(arguments, &backend, &result) ==
        expected_status);
    assert(fake.set_clock_calls == 0U);
    assert(fake.write_calls == 0U);
    assert(fake.read_calls == 0U);
    assert(ownership_fake.line_reset_calls == before.line_reset_calls);
    assert(ownership_fake.release_pins_calls == before.release_pins_calls);
}

static void test_invalid_input_does_not_touch_bus(void)
{
    assert_input_rejected("abc", AIRDAP_DEBUG_SHELL_SWD_PROBE_USAGE);
    assert_input_rejected("100 extra", AIRDAP_DEBUG_SHELL_SWD_PROBE_USAGE);
    assert_input_rejected("99", AIRDAP_DEBUG_SHELL_SWD_PROBE_CLOCK_OUT_OF_RANGE);
    assert_input_rejected("10001", AIRDAP_DEBUG_SHELL_SWD_PROBE_CLOCK_OUT_OF_RANGE);
    assert_input_rejected("42949672960", AIRDAP_DEBUG_SHELL_SWD_PROBE_CLOCK_OUT_OF_RANGE);
}

static void test_cancelled_probe_releases_diagnostic_owner(void)
{
    fake_swd_t fake = {
        .cancel_on_check = 3U,
    };
    const airdap_debug_shell_swd_backend_t backend = make_backend(&fake);
    airdap_debug_shell_swd_probe_result_t result;
    const ownership_counts_t before = ownership_counts();

    assert(airdap_debug_shell_swd_probe("100", &backend, &result) ==
        AIRDAP_DEBUG_SHELL_SWD_PROBE_CANCELLED);
    assert(fake.set_clock_calls == 1U);
    assert(fake.write_calls == 1U);
    assert(fake.read_calls == 0U);
    assert(fake.cancelled_calls == 3U);
    assert_diagnostic_owner_released(before);
}

static void test_set_clock_failure_is_reported_and_bus_is_released(void)
{
    fake_swd_t fake = {.set_clock_error = FAKE_SET_CLOCK_ERROR};
    const airdap_debug_shell_swd_backend_t backend = make_backend(&fake);
    airdap_debug_shell_swd_probe_result_t result;
    const ownership_counts_t before = ownership_counts();

    assert(airdap_debug_shell_swd_probe("100", &backend, &result) ==
        AIRDAP_DEBUG_SHELL_SWD_PROBE_SET_CLOCK_FAILED);
    assert(fake.write_calls == 0U);
    assert(fake.read_calls == 0U);
    assert(result.operation_error == FAKE_SET_CLOCK_ERROR);
    assert_diagnostic_owner_released(before);
}

static void test_sequence_write_failure_is_reported_and_bus_is_released(void)
{
    fake_swd_t fake = {.write_error = FAKE_CONNECT_ERROR};
    const airdap_debug_shell_swd_backend_t backend = make_backend(&fake);
    airdap_debug_shell_swd_probe_result_t result;
    const ownership_counts_t before = ownership_counts();

    assert(airdap_debug_shell_swd_probe("100", &backend, &result) ==
        AIRDAP_DEBUG_SHELL_SWD_PROBE_CONNECT_FAILED);
    assert(fake.write_calls == 1U);
    assert(fake.read_calls == 0U);
    assert(result.operation_error == FAKE_CONNECT_ERROR);
    assert_diagnostic_owner_released(before);
}

static void test_sequence_read_failure_is_reported_and_bus_is_released(void)
{
    fake_swd_t fake = {.read_error = FAKE_CONNECT_ERROR};
    const airdap_debug_shell_swd_backend_t backend = make_backend(&fake);
    airdap_debug_shell_swd_probe_result_t result;
    const ownership_counts_t before = ownership_counts();

    assert(airdap_debug_shell_swd_probe("100", &backend, &result) ==
        AIRDAP_DEBUG_SHELL_SWD_PROBE_CONNECT_FAILED);
    assert(fake.write_calls == 1U);
    assert(fake.read_calls == 1U);
    assert(result.operation_error == FAKE_CONNECT_ERROR);
    assert_diagnostic_owner_released(before);
}

static void test_raw_invalid_ack_is_reported(void)
{
    fake_swd_t fake = {
        .response_bits = make_response(7U, UINT32_C(0xFFFFFFFF), 1U),
    };
    const airdap_debug_shell_swd_backend_t backend = make_backend(&fake);
    airdap_debug_shell_swd_probe_result_t result;
    const ownership_counts_t before = ownership_counts();

    assert(airdap_debug_shell_swd_probe("100", &backend, &result) ==
        AIRDAP_DEBUG_SHELL_SWD_PROBE_RESPONSE_INVALID);
    assert(result.ack == 7U);
    assert(result.response_bits == fake.response_bits);
    assert_diagnostic_owner_released(before);
}

static void test_bad_idcode_parity_is_reported(void)
{
    fake_swd_t fake = {
        .response_bits = make_response(1U, UINT32_C(0x1BA01477), 1U),
    };
    const airdap_debug_shell_swd_backend_t backend = make_backend(&fake);
    airdap_debug_shell_swd_probe_result_t result;
    const ownership_counts_t before = ownership_counts();

    assert(airdap_debug_shell_swd_probe("100", &backend, &result) ==
        AIRDAP_DEBUG_SHELL_SWD_PROBE_PARITY_ERROR);
    assert(result.idcode == UINT32_C(0x1BA01477));
    assert(result.received_parity == 1U);
    assert(result.expected_parity == 0U);
    assert_diagnostic_owner_released(before);
}

static void test_release_failure_is_not_hidden(void)
{
    fake_swd_t fake = {
        .response_bits = make_response(1U, UINT32_C(0x2BA01477), 0U),
    };
    const airdap_debug_shell_swd_backend_t backend = make_backend(&fake);
    airdap_debug_shell_swd_probe_result_t result;
    const ownership_counts_t before = ownership_counts();

    ownership_fake.release_pins_success = false;

    assert(airdap_debug_shell_swd_probe("100", &backend, &result) ==
        AIRDAP_DEBUG_SHELL_SWD_PROBE_RELEASE_FAILED);
    assert(result.idcode == UINT32_C(0x2BA01477));
    assert(result.release_error == AIRDAP_DAP_OWNERSHIP_OFFLINE);
    assert(ownership_fake.line_reset_calls == before.line_reset_calls + 1U);
    assert(ownership_fake.release_pins_calls ==
        before.release_pins_calls + 1U);
}

int main(void)
{
    ownership_fake.release_pins_success = true;
    const airdap_dap_ownership_backend_t ownership_backend = {
        .context = &ownership_fake,
        .line_reset = fake_ownership_line_reset,
        .release_pins = fake_ownership_release_pins,
    };
    assert(airdap_dap_ownership_initialize(&ownership_backend) ==
        AIRDAP_DAP_OWNERSHIP_OK);

    test_existing_owners_block_probe_without_touching_swd();
    test_probe_blocks_revoke_and_releases_diagnostic_owner();
    test_default_clock_reads_idcode_and_releases_bus();
    test_stm32f1_hardware_capture_starts_with_ack();
    test_explicit_clock_is_parsed();
    test_invalid_input_does_not_touch_bus();
    test_cancelled_probe_releases_diagnostic_owner();
    test_set_clock_failure_is_reported_and_bus_is_released();
    test_sequence_write_failure_is_reported_and_bus_is_released();
    test_sequence_read_failure_is_reported_and_bus_is_released();
    test_raw_invalid_ack_is_reported();
    test_bad_idcode_parity_is_reported();
    test_release_failure_is_not_hidden();
    return 0;
}
