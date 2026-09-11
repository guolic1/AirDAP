#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "airdap_board.h"
#include "airdap_control.h"
#include "airdap_mode_state.h"
#include "airdap_network_control.h"

struct airdap_network_auth_connection { unsigned id; };
static struct airdap_network_auth_connection connection = {1U};
static struct airdap_network_auth_connection other_connection = {2U};
static unsigned validation_calls;
static unsigned reject_validation;
static unsigned board_calls;
static unsigned line_reset_calls;
static unsigned release_calls;
static uint8_t last_opcode;
static bool last_value;
static bool power_status;
static esp_err_t board_result;
static bool board_reset;
static bool board_power;

airdap_network_auth_result_t airdap_network_auth_session_validate(
    airdap_network_auth_connection_t *candidate, uint32_t session_id)
{
    ++validation_calls;
    if (candidate != &connection || session_id != 77U ||
        validation_calls == reject_validation) {
        return AIRDAP_NETWORK_AUTH_UNAUTHENTICATED;
    }
    return AIRDAP_NETWORK_AUTH_OK;
}

static void board_event(uint8_t opcode, bool value)
{
    assert(validation_calls == 2U);
    ++board_calls;
    last_opcode = opcode;
    last_value = value;
    /* A fake board callback is inside the actual operation reservation.
     * Neither another controller, DAP acquisition nor OTA can enter here. */
    airdap_dap_ownership_operation_t competing = {0};
    airdap_dap_ownership_claim_t claim = {0};
    assert(airdap_dap_ownership_control_begin(AIRDAP_DAP_OWNER_NETWORK,
        &competing) == AIRDAP_DAP_OWNERSHIP_BUSY);
    assert(airdap_dap_ownership_acquire(AIRDAP_DAP_OWNER_USB, &claim) ==
        AIRDAP_DAP_OWNERSHIP_BUSY);
    assert(airdap_dap_ownership_suspend() == AIRDAP_DAP_OWNERSHIP_BUSY);
    assert(airdap_dap_ownership_revoke() == AIRDAP_DAP_OWNERSHIP_BUSY);
}

esp_err_t airdap_target_reset_set_asserted(bool asserted)
{
    board_event(AIRDAP_CONTROL_RESET_SET, asserted);
    if (board_result == ESP_OK) { board_reset = asserted; }
    return board_result;
}

esp_err_t airdap_target_power_set_allowed(bool allowed)
{
    board_event(AIRDAP_CONTROL_POWER_SET, allowed);
    if (board_result == ESP_OK) { board_power = allowed; }
    return board_result;
}

esp_err_t airdap_target_power_get_active(bool *active)
{
    board_event(AIRDAP_CONTROL_POWER_GET, power_status);
    *active = power_status;
    return board_result;
}

static bool line_reset(void *context)
{
    (void) context;
    ++line_reset_calls;
    return true;
}

static bool release_pins(void *context)
{
    (void) context;
    ++release_calls;
    return true;
}

static void reset_test(void)
{
    assert(airdap_dap_ownership_revoke() == AIRDAP_DAP_OWNERSHIP_OK);
    airdap_mode_state_init();
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_CONNECTING) ==
        AIRDAP_MODE_STATE_OK);
    assert(airdap_mode_state_transition(AIRDAP_MODE_EVENT_WIFI_ONLINE) ==
        AIRDAP_MODE_STATE_OK);
    validation_calls = 0U;
    reject_validation = 0U;
    board_calls = 0U;
    line_reset_calls = 0U;
    release_calls = 0U;
    board_result = ESP_OK;
    board_reset = false;
    board_power = false;
    power_status = false;
}

static void expect(const uint8_t *payload, size_t size,
    airdap_frame_error_code_t expected)
{
    uint8_t response[2] = {0xA5U, 0xA5U};
    size_t response_size = 99U;
    const unsigned calls_before = board_calls;
    assert(airdap_network_control_dispatch(&connection, 77U,
        payload, size, response, &response_size) == expected);
    if (expected == AIRDAP_FRAME_ERROR_NONE) {
        assert(response_size == 2U);
        assert(response[0] == payload[0]);
        assert(response[1] == (size == 1U ? power_status : payload[1]));
        assert(board_calls == calls_before + 1U);
    } else {
        assert(response_size == 0U);
        assert(response[0] == 0xA5U && response[1] == 0xA5U);
    }
    /* All success/error exits must release the reservation. */
    airdap_dap_ownership_operation_t next = {0};
    if (airdap_mode_state_control_operation_begin(true, &next) ==
        AIRDAP_MODE_DAP_ALLOWED) {
        airdap_dap_ownership_operation_end(&next);
    }
}

static void test_set_and_get(void)
{
    const uint8_t opcodes[] = {0x20U, 0x21U, 0x22U};
    for (size_t index = 0U; index < sizeof(opcodes); ++index) {
        for (uint8_t value = 0U; value <= 1U; ++value) {
            reset_test();
            power_status = value != 0U;
            const uint8_t payload[] = {opcodes[index], value};
            expect(payload, index == 2U ? 1U : 2U, AIRDAP_FRAME_ERROR_NONE);
            assert(last_opcode == payload[0] && last_value == (value != 0U));
            assert(line_reset_calls == 0U && release_calls == 0U);
            assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_NONE);
            if (index == 0U) { assert(board_reset == (value != 0U)); }
            if (index == 1U) { assert(board_power == (value != 0U)); }
        }
    }
    reset_test();
    const uint8_t allow[] = {0x21U, 1U};
    expect(allow, sizeof(allow), AIRDAP_FRAME_ERROR_NONE);
    assert(board_power && !power_status);
    validation_calls = 0U;
    const uint8_t get[] = {0x22U};
    expect(get, sizeof(get), AIRDAP_FRAME_ERROR_NONE);
    assert(!last_value); /* Acknowledged permission is not sampled power. */
}

static void test_authentication_and_payload_errors(void)
{
    const uint8_t request[] = {0x20U, 1U};
    for (unsigned rejected = 1U; rejected <= 2U; ++rejected) {
        reset_test();
        reject_validation = rejected;
        expect(request, sizeof(request), AIRDAP_FRAME_ERROR_UNAUTHENTICATED);
        assert(board_calls == 0U && line_reset_calls == 0U);
    }
    reset_test();
    uint8_t response[2];
    size_t size = 0U;
    assert(airdap_network_control_dispatch(&other_connection, 77U, request,
        sizeof(request), response, &size) == AIRDAP_FRAME_ERROR_UNAUTHENTICATED);
    assert(airdap_network_control_dispatch(&connection, 78U, request,
        sizeof(request), response, &size) == AIRDAP_FRAME_ERROR_UNAUTHENTICATED);
    assert(airdap_network_control_dispatch(&connection, 0U, request,
        sizeof(request), response, &size) == AIRDAP_FRAME_ERROR_UNAUTHENTICATED);
    assert(airdap_network_control_dispatch(NULL, 77U, request,
        sizeof(request), response, &size) == AIRDAP_FRAME_ERROR_UNAUTHENTICATED);
    assert(board_calls == 0U);

    const struct { uint8_t data[3]; size_t size; airdap_frame_error_code_t error; }
        cases[] = {
            {{0}, 0U, AIRDAP_FRAME_ERROR_TRUNCATED},
            {{0x20}, 1U, AIRDAP_FRAME_ERROR_TRUNCATED},
            {{0x21}, 1U, AIRDAP_FRAME_ERROR_TRUNCATED},
            {{0x20, 2}, 2U, AIRDAP_FRAME_ERROR_INVALID_ARGUMENT},
            {{0x21, 255}, 2U, AIRDAP_FRAME_ERROR_INVALID_ARGUMENT},
            {{0x20, 1, 0}, 3U, AIRDAP_FRAME_ERROR_INVALID_ARGUMENT},
            {{0x21, 1, 0}, 3U, AIRDAP_FRAME_ERROR_INVALID_ARGUMENT},
            {{0x22, 0}, 2U, AIRDAP_FRAME_ERROR_INVALID_ARGUMENT},
            {{0x10}, 1U, AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE},
            {{0xff}, 1U, AIRDAP_FRAME_ERROR_UNSUPPORTED_TYPE},
        };
    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        reset_test();
        expect(cases[index].data, cases[index].size, cases[index].error);
        assert(board_calls == 0U && line_reset_calls == 0U);
    }
}

static void test_mode_ownership_and_board_failure(void)
{
    const uint8_t request[] = {0x21U, 1U};
    const airdap_mode_event_t denied[] = {
        AIRDAP_MODE_EVENT_USB_ATTACHED, AIRDAP_MODE_EVENT_WIFI_DISCONNECTED,
        AIRDAP_MODE_EVENT_WIFI_STOPPED, AIRDAP_MODE_EVENT_OTA_STARTED,
    };
    for (size_t index = 0U; index < sizeof(denied) / sizeof(denied[0]); ++index) {
        reset_test();
        assert(airdap_mode_state_transition(denied[index]) == AIRDAP_MODE_STATE_OK);
        expect(request, sizeof(request), AIRDAP_FRAME_ERROR_BUSY);
        assert(board_calls == 0U && line_reset_calls == 0U);
    }
    for (airdap_dap_owner_t owner = AIRDAP_DAP_OWNER_USB;
         owner <= AIRDAP_DAP_OWNER_DIAGNOSTIC; ++owner) {
        reset_test();
        airdap_dap_ownership_claim_t claim = {0};
        assert(airdap_dap_ownership_acquire(owner, &claim) == AIRDAP_DAP_OWNERSHIP_OK);
        expect(request, sizeof(request), AIRDAP_FRAME_ERROR_BUSY);
        assert(board_calls == 0U);
        if (owner == AIRDAP_DAP_OWNER_NETWORK) {
            validation_calls = 0U;
            const uint8_t reset[] = {0x20U, 1U};
            expect(reset, sizeof(reset), AIRDAP_FRAME_ERROR_NONE);
        }
        assert(airdap_dap_ownership_current() == owner);
        assert(line_reset_calls == 1U && release_calls == 0U);
    }
    reset_test();
    assert(airdap_dap_ownership_suspend() == AIRDAP_DAP_OWNERSHIP_OK);
    expect(request, sizeof(request), AIRDAP_FRAME_ERROR_BUSY);
    assert(board_calls == 0U);
    assert(airdap_dap_ownership_resume() == AIRDAP_DAP_OWNERSHIP_OK);

    reset_test();
    airdap_dap_ownership_claim_t claim = {0};
    airdap_dap_ownership_operation_t active = {0};
    assert(airdap_dap_ownership_acquire(AIRDAP_DAP_OWNER_NETWORK, &claim) ==
        AIRDAP_DAP_OWNERSHIP_OK);
    assert(airdap_dap_ownership_operation_begin(&claim, &active) ==
        AIRDAP_DAP_OWNERSHIP_OK);
    expect(request, sizeof(request), AIRDAP_FRAME_ERROR_BUSY);
    assert(board_calls == 0U);
    airdap_dap_ownership_operation_end(&active);

    for (uint8_t opcode = 0x20U; opcode <= 0x22U; ++opcode) {
        reset_test();
        board_result = ESP_FAIL;
        const uint8_t failed[] = {opcode, 1U};
        expect(failed, opcode == 0x22U ? 1U : 2U, AIRDAP_FRAME_ERROR_INTERNAL);
        assert(board_calls == 1U && !board_reset && !board_power);
        airdap_dap_ownership_operation_t next = {0};
        assert(airdap_mode_state_control_operation_begin(true, &next) ==
            AIRDAP_MODE_DAP_ALLOWED);
        airdap_dap_ownership_operation_end(&next);
    }
}

int main(void)
{
    const airdap_dap_ownership_backend_t backend = {
        .line_reset = line_reset, .release_pins = release_pins,
    };
    assert(airdap_dap_ownership_initialize(&backend) == AIRDAP_DAP_OWNERSHIP_OK);
    test_set_and_get();
    test_authentication_and_payload_errors();
    test_mode_ownership_and_board_failure();
    puts("Authenticated network control tests passed");
    return 0;
}
