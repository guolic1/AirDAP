#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include "freertos/semphr.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_dap_ownership.h"
#include "airdap_device_identity.h"
#include "airdap_mode_state.h"
#include "airdap_ota.h"
#include "airdap_network_ota.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

enum {
    UPDATE_PARTITION_SIZE = 0x3F0000,
    UPDATE_HANDLE = 42,
};

static const esp_partition_t running_partition = {
    .address = 0x20000,
    .size = UPDATE_PARTITION_SIZE,
    .type = 0,
    .subtype = 0x10,
    .label = "ota_0",
};
static const esp_partition_t update_partition = {
    .address = 0x410000,
    .size = UPDATE_PARTITION_SIZE,
    .type = 0,
    .subtype = 0x11,
    .label = "ota_1",
};
static const airdap_device_identity_t device_identity = {
    .firmware_version = "1.2.3-test",
};

static bool network_authorized = true;
static bool revoke_on_end;
static unsigned uart_suspend_calls;
static bool uart_suspended;
static esp_err_t uart_suspend_result;

airdap_network_auth_result_t airdap_network_auth_session_validate(
    airdap_network_auth_connection_t *connection, uint32_t session_id)
{
    return connection != NULL && session_id == 7 && network_authorized
        ? AIRDAP_NETWORK_AUTH_OK : AIRDAP_NETWORK_AUTH_UNAUTHENTICATED;
}

static esp_err_t begin_result;
static esp_err_t write_result;
static esp_err_t end_result;
static esp_err_t abort_result;
static esp_err_t activate_result;
static esp_err_t state_result;
static esp_err_t confirm_result;
static esp_ota_img_states_t running_state;
static bool return_device_identity;
static bool return_update_partition;
static bool restarted;
static unsigned begin_calls;
static unsigned write_calls;
static unsigned end_calls;
static unsigned abort_calls;
static unsigned activate_calls;
static unsigned confirm_calls;
static size_t begun_size;
static size_t last_write_size;
static esp_ota_handle_t last_handle;
static const esp_partition_t *activated_partition;
static bool ownership_release_succeeds;
static unsigned ownership_line_reset_calls;
static unsigned ownership_release_calls;

static airdap_mode_ota_state_t mode_ota_state(void)
{
    airdap_mode_snapshot_t state;
    assert(airdap_mode_state_get(&state) == AIRDAP_MODE_STATE_OK);
    return state.ota;
}

static void reset_fakes(void)
{
    network_authorized = true;
    revoke_on_end = false;
    uart_suspend_calls = 0;
    uart_suspend_result = ESP_OK;
    begin_result = ESP_OK;
    write_result = ESP_OK;
    end_result = ESP_OK;
    abort_result = ESP_OK;
    activate_result = ESP_OK;
    state_result = ESP_OK;
    confirm_result = ESP_OK;
    running_state = ESP_OTA_IMG_VALID;
    return_device_identity = true;
    return_update_partition = true;
    restarted = false;
    begin_calls = 0U;
    write_calls = 0U;
    end_calls = 0U;
    abort_calls = 0U;
    activate_calls = 0U;
    confirm_calls = 0U;
    begun_size = 0U;
    last_write_size = 0U;
    last_handle = 0U;
    activated_partition = NULL;
    ownership_release_succeeds = true;
    ownership_line_reset_calls = 0U;
    ownership_release_calls = 0U;
    assert(airdap_ota_initialize() == ESP_OK);
}

static bool ownership_line_reset(void *context)
{
    (void) context;
    ++ownership_line_reset_calls;
    return true;
}

static bool ownership_release_pins(void *context)
{
    (void) context;
    assert(mode_ota_state() == AIRDAP_OTA_RECEIVING);
    ++ownership_release_calls;
    return ownership_release_succeeds;
}

const esp_partition_t *esp_ota_get_next_update_partition(
    const esp_partition_t *start_from)
{
    assert(start_from == NULL);
    return return_update_partition ? &update_partition : NULL;
}

const esp_partition_t *esp_ota_get_running_partition(void)
{
    return &running_partition;
}

const airdap_device_identity_t *airdap_device_identity_get(void)
{
    return return_device_identity ? &device_identity : NULL;
}

esp_err_t esp_ota_get_state_partition(
    const esp_partition_t *partition,
    esp_ota_img_states_t *state)
{
    assert(partition == &running_partition);
    assert(state != NULL);
    *state = running_state;
    return state_result;
}

esp_err_t esp_ota_begin(
    const esp_partition_t *partition,
    size_t image_size,
    esp_ota_handle_t *out_handle)
{
    assert(partition == &update_partition);
    assert(out_handle != NULL);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_NONE);
    assert(mode_ota_state() == AIRDAP_OTA_RECEIVING);
    airdap_dap_ownership_claim_t blocked_claim = {0};
    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_NETWORK,
        &blocked_claim) == AIRDAP_DAP_OWNERSHIP_BUSY);
    assert(uart_suspended);
    assert(uart_suspend_calls == begin_calls + 1);
    ++begin_calls;
    begun_size = image_size;
    *out_handle = UPDATE_HANDLE;
    return begin_result;
}

esp_err_t esp_ota_write(
    esp_ota_handle_t handle,
    const void *data,
    size_t size)
{
    assert(handle == UPDATE_HANDLE);
    assert(data != NULL);
    ++write_calls;
    last_write_size = size;
    last_handle = handle;
    return write_result;
}

esp_err_t esp_ota_end(esp_ota_handle_t handle)
{
    assert(handle == UPDATE_HANDLE);
    if (revoke_on_end) network_authorized = false;
    ++end_calls;
    last_handle = handle;
    return end_result;
}

esp_err_t esp_ota_abort(esp_ota_handle_t handle)
{
    assert(handle == UPDATE_HANDLE);
    ++abort_calls;
    last_handle = handle;
    return abort_result;
}

esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition)
{
    ++activate_calls;
    activated_partition = partition;
    return activate_result;
}

esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    ++confirm_calls;
    return confirm_result;
}

void esp_restart(void)
{
    restarted = true;
}

static void test_reports_update_capacity_and_running_version(void)
{
    reset_fakes();
    airdap_ota_info_t info = {0};

    assert(airdap_ota_get_info(&info) == AIRDAP_OTA_STATUS_OK);
    assert(info.protocol_version == AIRDAP_OTA_PROTOCOL_VERSION);
    assert((info.flags & AIRDAP_OTA_FLAG_ROLLBACK) != 0U);
    assert(info.max_image_size == UPDATE_PARTITION_SIZE);
    assert(strcmp(info.running_version, "1.2.3-test") == 0);

    return_update_partition = false;
    assert(airdap_ota_get_info(&info) == AIRDAP_OTA_STATUS_INTERNAL_ERROR);
    return_update_partition = true;
    return_device_identity = false;
    assert(airdap_ota_get_info(&info) == AIRDAP_OTA_STATUS_INTERNAL_ERROR);
    assert(airdap_ota_get_info(NULL) == AIRDAP_OTA_STATUS_INVALID_ARGUMENT);
}

static void test_successful_sequential_update_commits_then_reboots(void)
{
    static const uint8_t first[60] = {1U};
    static const uint8_t second[40] = {2U};
    airdap_dap_ownership_claim_t usb_claim = {0};
    reset_fakes();
    uint32_t next_offset = UINT32_MAX;

    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_USB,
        &usb_claim) ==
        AIRDAP_DAP_OWNERSHIP_OK);
    assert(airdap_ota_begin(100U) == AIRDAP_OTA_STATUS_OK);
    assert(!airdap_ota_debug_allowed());
    assert(mode_ota_state() == AIRDAP_OTA_RECEIVING);
    assert(ownership_line_reset_calls == 1U);
    assert(ownership_release_calls == 1U);
    assert(begin_calls == 1U && begun_size == OTA_WITH_SEQUENTIAL_WRITES);
    assert(airdap_ota_write(0U, first, sizeof(first), &next_offset) ==
        AIRDAP_OTA_STATUS_OK);
    assert(next_offset == 60U);
    assert(airdap_ota_write(60U, second, sizeof(second), &next_offset) ==
        AIRDAP_OTA_STATUS_OK);
    assert(next_offset == 100U);
    assert(write_calls == 2U && last_write_size == sizeof(second));

    assert(airdap_ota_commit() == AIRDAP_OTA_STATUS_OK);
    assert(mode_ota_state() == AIRDAP_OTA_READY_TO_REBOOT);
    assert(end_calls == 1U);
    assert(activate_calls == 1U && activated_partition == &update_partition);
    assert(airdap_ota_reboot() == AIRDAP_OTA_STATUS_OK);
    assert(restarted);
    assert(airdap_ota_begin(10U) == AIRDAP_OTA_STATUS_INVALID_STATE);
}

static void test_rejects_invalid_sizes_offsets_and_arguments(void)
{
    static const uint8_t data[16] = {0U};
    airdap_dap_ownership_claim_t usb_claim = {0};
    reset_fakes();
    uint32_t next_offset = 0U;

    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_USB,
        &usb_claim) ==
        AIRDAP_DAP_OWNERSHIP_OK);
    assert(airdap_ota_begin(0U) == AIRDAP_OTA_STATUS_INVALID_SIZE);
    assert(airdap_ota_begin(UPDATE_PARTITION_SIZE + 1U) ==
        AIRDAP_OTA_STATUS_INVALID_SIZE);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_USB);
    assert(ownership_release_calls == 0U && begin_calls == 0U);

    assert(airdap_ota_begin(sizeof(data)) == AIRDAP_OTA_STATUS_OK);
    assert(ownership_release_calls == 1U);
    assert(airdap_ota_begin(sizeof(data)) == AIRDAP_OTA_STATUS_INVALID_STATE);
    assert(airdap_ota_write(1U, data, sizeof(data), &next_offset) ==
        AIRDAP_OTA_STATUS_INVALID_OFFSET);
    assert(airdap_ota_write(0U, NULL, sizeof(data), &next_offset) ==
        AIRDAP_OTA_STATUS_INVALID_ARGUMENT);
    assert(airdap_ota_write(0U, data, 0U, &next_offset) ==
        AIRDAP_OTA_STATUS_INVALID_SIZE);
    assert(airdap_ota_write(0U, data, sizeof(data) + 1U, &next_offset) ==
        AIRDAP_OTA_STATUS_INVALID_SIZE);
    assert(write_calls == 0U);
    assert(airdap_ota_abort() == AIRDAP_OTA_STATUS_OK);
    assert(mode_ota_state() == AIRDAP_OTA_IDLE);
}

static void test_begin_requires_successful_owner_revoke(void)
{
    airdap_dap_ownership_operation_t operation = {0};
    airdap_dap_ownership_claim_t usb_claim = {0};

    reset_fakes();
    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_USB,
        &usb_claim) ==
        AIRDAP_DAP_OWNERSHIP_OK);
    assert(airdap_dap_ownership_operation_begin(
        &usb_claim,
        &operation) == AIRDAP_DAP_OWNERSHIP_OK);
    assert(airdap_ota_begin(32U) == AIRDAP_OTA_STATUS_INVALID_STATE);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_USB);
    assert(mode_ota_state() == AIRDAP_OTA_IDLE);
    assert(ownership_release_calls == 0U && begin_calls == 0U);
    assert(airdap_ota_debug_allowed());

    airdap_dap_ownership_operation_end(&operation);
    assert(airdap_ota_begin(32U) == AIRDAP_OTA_STATUS_OK);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_NONE);
    assert(ownership_release_calls == 1U && begin_calls == 1U);
    assert(airdap_ota_abort() == AIRDAP_OTA_STATUS_OK);
}

static void test_begin_fails_if_physical_release_fails(void)
{
    airdap_dap_ownership_claim_t usb_claim = {0};
    airdap_dap_ownership_claim_t network_claim = {0};

    reset_fakes();
    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_USB,
        &usb_claim) ==
        AIRDAP_DAP_OWNERSHIP_OK);
    ownership_release_succeeds = false;
    assert(airdap_ota_begin(32U) == AIRDAP_OTA_STATUS_INTERNAL_ERROR);
    assert(ownership_release_calls == 1U && begin_calls == 0U);
    assert(airdap_dap_ownership_current() == AIRDAP_DAP_OWNER_NONE);
    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_NETWORK,
        &network_claim) ==
        AIRDAP_DAP_OWNERSHIP_OFFLINE);
    assert(airdap_ota_debug_allowed());
    assert(mode_ota_state() == AIRDAP_OTA_IDLE);
}

static void test_begin_failure_releases_handle_created_by_idf(void)
{
    reset_fakes();
    begin_result = ESP_FAIL;

    assert(airdap_ota_begin(32U) == AIRDAP_OTA_STATUS_INTERNAL_ERROR);
    assert(begin_calls == 1U);
    assert(abort_calls == 1U && last_handle == UPDATE_HANDLE);
    assert(airdap_ota_debug_allowed());
    assert(mode_ota_state() == AIRDAP_OTA_IDLE);
}

static void test_partial_commit_can_receive_remaining_bytes(void)
{
    static const uint8_t data[10] = {0U};
    reset_fakes();
    uint32_t next_offset = 0U;

    assert(airdap_ota_begin(20U) == AIRDAP_OTA_STATUS_OK);
    assert(airdap_ota_write(0U, data, sizeof(data), &next_offset) ==
        AIRDAP_OTA_STATUS_OK);
    assert(airdap_ota_commit() == AIRDAP_OTA_STATUS_INCOMPLETE_IMAGE);
    assert(end_calls == 0U && activate_calls == 0U);
    assert(airdap_ota_write(10U, data, sizeof(data), &next_offset) ==
        AIRDAP_OTA_STATUS_OK);
    assert(airdap_ota_commit() == AIRDAP_OTA_STATUS_OK);
}

static void test_write_or_validation_failure_never_activates(void)
{
    static const uint8_t data[8] = {0U};
    uint32_t next_offset = 0U;

    reset_fakes();
    assert(airdap_ota_begin(sizeof(data)) == AIRDAP_OTA_STATUS_OK);
    write_result = ESP_FAIL;
    assert(airdap_ota_write(0U, data, sizeof(data), &next_offset) ==
        AIRDAP_OTA_STATUS_WRITE_FAILED);
    assert(mode_ota_state() == AIRDAP_OTA_IDLE);
    assert(abort_calls == 1U && activate_calls == 0U);
    assert(airdap_ota_commit() == AIRDAP_OTA_STATUS_INVALID_STATE);

    reset_fakes();
    assert(airdap_ota_begin(sizeof(data)) == AIRDAP_OTA_STATUS_OK);
    assert(airdap_ota_write(0U, data, sizeof(data), &next_offset) ==
        AIRDAP_OTA_STATUS_OK);
    end_result = ESP_ERR_OTA_VALIDATE_FAILED;
    assert(airdap_ota_commit() == AIRDAP_OTA_STATUS_VALIDATION_FAILED);
    assert(mode_ota_state() == AIRDAP_OTA_IDLE);
    assert(activate_calls == 0U);

    reset_fakes();
    assert(airdap_ota_begin(sizeof(data)) == AIRDAP_OTA_STATUS_OK);
    assert(airdap_ota_write(0U, data, sizeof(data), &next_offset) ==
        AIRDAP_OTA_STATUS_OK);
    activate_result = ESP_FAIL;
    assert(airdap_ota_commit() == AIRDAP_OTA_STATUS_ACTIVATION_FAILED);
    assert(mode_ota_state() == AIRDAP_OTA_IDLE);
    assert(airdap_ota_reboot() == AIRDAP_OTA_STATUS_INVALID_STATE);
    assert(!restarted);
}

static void test_abort_and_disconnect_release_only_live_sessions(void)
{
    reset_fakes();
    assert(airdap_ota_debug_allowed());
    assert(airdap_ota_abort() == AIRDAP_OTA_STATUS_OK);
    assert(abort_calls == 0U);

    assert(airdap_ota_begin(32U) == AIRDAP_OTA_STATUS_OK);
    airdap_ota_handle_disconnect();
    assert(abort_calls == 1U);
    assert(airdap_ota_debug_allowed());
    assert(mode_ota_state() == AIRDAP_OTA_IDLE);
    assert(airdap_ota_commit() == AIRDAP_OTA_STATUS_INVALID_STATE);
}

static void test_abort_failure_keeps_debug_blocked_until_retry_succeeds(void)
{
    airdap_dap_ownership_claim_t claim = {0};

    reset_fakes();
    assert(airdap_ota_begin(32U) == AIRDAP_OTA_STATUS_OK);
    abort_result = ESP_FAIL;
    assert(airdap_ota_abort() == AIRDAP_OTA_STATUS_INTERNAL_ERROR);
    assert(abort_calls == 1U);
    assert(mode_ota_state() == AIRDAP_OTA_RECEIVING);
    assert(!airdap_ota_debug_allowed());
    assert(airdap_dap_ownership_acquire(
        AIRDAP_DAP_OWNER_NETWORK,
        &claim) == AIRDAP_DAP_OWNERSHIP_BUSY);

    abort_result = ESP_OK;
    assert(airdap_ota_abort() == AIRDAP_OTA_STATUS_OK);
    assert(abort_calls == 2U);
    assert(mode_ota_state() == AIRDAP_OTA_IDLE);
    assert(airdap_ota_debug_allowed());
}

static void test_write_abort_failure_keeps_debug_blocked(void)
{
    static const uint8_t data[8] = {0U};
    uint32_t next_offset = 0U;

    reset_fakes();
    assert(airdap_ota_begin(sizeof(data)) == AIRDAP_OTA_STATUS_OK);
    write_result = ESP_FAIL;
    abort_result = ESP_FAIL;
    assert(airdap_ota_write(0U, data, sizeof(data), &next_offset) ==
        AIRDAP_OTA_STATUS_INTERNAL_ERROR);
    assert(abort_calls == 1U);
    assert(mode_ota_state() == AIRDAP_OTA_RECEIVING);
    assert(!airdap_ota_debug_allowed());

    abort_result = ESP_OK;
    assert(airdap_ota_abort() == AIRDAP_OTA_STATUS_OK);
    assert(mode_ota_state() == AIRDAP_OTA_IDLE);
    assert(airdap_ota_debug_allowed());
}

static void test_pending_image_is_confirmed_only_when_requested(void)
{
    reset_fakes();
    running_state = ESP_OTA_IMG_VALID;
    assert(airdap_ota_confirm_running_image() == ESP_OK);
    assert(confirm_calls == 0U);

    running_state = ESP_OTA_IMG_PENDING_VERIFY;
    assert(airdap_ota_confirm_running_image() == ESP_OK);
    assert(confirm_calls == 1U);

    confirm_result = ESP_FAIL;
    assert(airdap_ota_confirm_running_image() == ESP_FAIL);
    assert(confirm_calls == 2U);

    state_result = ESP_ERR_INVALID_STATE;
    assert(airdap_ota_confirm_running_image() == ESP_ERR_INVALID_STATE);
    assert(confirm_calls == 2U);
}

static bool client_authorized(void *context, uint32_t session_id)
{
    assert(session_id == 7);
    return *(bool *) context;
}

static void test_network_owner_isolated_from_usb_and_other_connections(void)
{
    reset_fakes();
    bool authorized = true, other = true;
    const airdap_ota_client_t client = {&authorized, 7, client_authorized};
    const airdap_ota_client_t stranger = {&other, 7, client_authorized};
    assert(airdap_ota_begin_for_client(&client, 8) == AIRDAP_OTA_STATUS_OK);
    assert(airdap_ota_abort() == AIRDAP_OTA_STATUS_BUSY);
    assert(airdap_ota_abort_for_client(&stranger) == AIRDAP_OTA_STATUS_BUSY);
    airdap_ota_handle_disconnect();
    assert(mode_ota_state() == AIRDAP_OTA_RECEIVING);
    authorized = false;
    const uint8_t data[8] = {0};
    uint32_t offset;
    assert(airdap_ota_write_for_client(&client, 0, data, 8, &offset) == AIRDAP_OTA_STATUS_UNAUTHENTICATED);
    assert(write_calls == 0);
    airdap_ota_disconnect_client(&client);
    assert(abort_calls == 1);
    assert(activate_calls == 0);
    assert(airdap_ota_debug_allowed());
}

static uint8_t ota_response[AIRDAP_NETWORK_OTA_MAX_RESPONSE];
static size_t ota_response_size;
static int connection_marker;
static airdap_frame_error_code_t dispatch(const uint8_t *data, size_t size)
{
    return airdap_network_ota_dispatch((void *) &connection_marker, 7,
        data, size, ota_response, sizeof(ota_response), &ota_response_size);
}

static void test_network_protocol_and_lifecycle(void)
{
    const uint8_t query[] = {0x30};
    const uint8_t begin[] = {0x31, 0, 0, 0, 8};
    uint8_t write[] = {0x32, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
    const uint8_t commit[] = {0x33}, abort[] = {0x34}, reboot[] = {0x35};
    reset_fakes();
    network_authorized = false;
    for (uint8_t opcode = 0x30; opcode <= 0x35; ++opcode) {
        assert(dispatch(&opcode, 1) == AIRDAP_FRAME_ERROR_UNAUTHENTICATED);
        assert(ota_response_size == 0);
    }
    assert(begin_calls == 0 && write_calls == 0 && activate_calls == 0);
    network_authorized = true;
    assert(dispatch(query, 1) == AIRDAP_FRAME_ERROR_NONE);
    assert(ota_response_size == 17 + strlen("1.2.3-test"));
    assert(ota_response[0] == 0x30 && ota_response[1] == 0);
    assert(ota_response[2] == 1 && ota_response[3] == 1 && ota_response[16] == 1);
    assert(dispatch(begin, 4) == AIRDAP_FRAME_ERROR_TRUNCATED);
    assert(dispatch(begin, 5) == AIRDAP_FRAME_ERROR_NONE && ota_response[1] == 0);
    assert(uart_suspended);
    assert(dispatch(commit, 1) == AIRDAP_FRAME_ERROR_NONE && ota_response[1] == 5);
    assert(activate_calls == 0);
    write[4] = 1;
    assert(dispatch(write, sizeof(write)) == AIRDAP_FRAME_ERROR_NONE && ota_response[1] == 4);
    assert(write_calls == 0);
    write[4] = 0;
    assert(dispatch(write, sizeof(write)) == AIRDAP_FRAME_ERROR_NONE);
    const uint8_t expected[] = {0x32, 0, 0, 0, 0, 8};
    assert(ota_response_size == sizeof(expected) && memcmp(ota_response, expected, sizeof(expected)) == 0);
    assert(dispatch(commit, 1) == AIRDAP_FRAME_ERROR_NONE && ota_response[1] == 0);
    assert(activate_calls == 1 && activated_partition == &update_partition && confirm_calls == 0);
    assert(dispatch(commit, 1) == AIRDAP_FRAME_ERROR_NONE && ota_response[1] == 2);
    assert(dispatch(abort, 1) == AIRDAP_FRAME_ERROR_NONE && ota_response[1] == 2);
    airdap_network_ota_disconnect((void *) &connection_marker, 7);
    assert(uart_suspended && abort_calls == 0);
    assert(dispatch(reboot, 1) == AIRDAP_FRAME_ERROR_NONE && ota_response[1] == 0);
    assert(!restarted); /* ACK is constructed before restarting. */
    airdap_network_ota_reboot((void *) &connection_marker, 7);
    assert(restarted);

    reset_fakes();
    assert(dispatch(begin, 5) == AIRDAP_FRAME_ERROR_NONE && ota_response[1] == 0);
    assert(dispatch(abort, 1) == AIRDAP_FRAME_ERROR_NONE && ota_response[1] == 0);
    assert(!uart_suspended && activate_calls == 0 && abort_calls == 1);
    assert(dispatch(begin, 5) == AIRDAP_FRAME_ERROR_NONE && ota_response[1] == 0);
    airdap_network_ota_disconnect((void *) &connection_marker, 7);
    assert(!uart_suspended && activate_calls == 0 && abort_calls == 2);
}

static void test_revocation_during_commit_cannot_activate(void)
{
    reset_fakes();
    const uint8_t begin[] = {0x31, 0, 0, 0, 1}, write[] = {0x32, 0, 0, 0, 0, 0xe9}, commit[] = {0x33};
    assert(dispatch(begin, sizeof(begin)) == AIRDAP_FRAME_ERROR_NONE);
    assert(dispatch(write, sizeof(write)) == AIRDAP_FRAME_ERROR_NONE);
    revoke_on_end = true;
    assert(dispatch(commit, 1) == AIRDAP_FRAME_ERROR_UNAUTHENTICATED);
    assert(end_calls == 1 && activate_calls == 0 && !uart_suspended);
    assert(airdap_ota_debug_allowed());
}

static void test_uart_drain_failure_prevents_flash_begin(void)
{
    reset_fakes();
    uart_suspend_result = ESP_FAIL;
    assert(airdap_ota_begin(8) == AIRDAP_OTA_STATUS_BUSY);
    assert(begin_calls == 0 && !uart_suspended && airdap_ota_debug_allowed());
}

typedef struct {
    airdap_ota_client_t client;
    airdap_ota_status_t result;
} concurrent_begin_t;

static void *concurrent_begin(void *argument)
{
    concurrent_begin_t *call = argument;
    call->result = airdap_ota_begin_for_client(&call->client, 8);
    return NULL;
}

static void test_concurrent_clients_cannot_share_flash_handle(void)
{
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        reset_fakes();
        bool a = true, b = true;
        concurrent_begin_t calls[2] = {{{&a, 7, client_authorized}, 0}, {{&b, 7, client_authorized}, 0}};
        pthread_t workers[2];
        for (unsigned i = 0; i < 2; ++i) assert(pthread_create(&workers[i], NULL, concurrent_begin, &calls[i]) == 0);
        for (unsigned i = 0; i < 2; ++i) assert(pthread_join(workers[i], NULL) == 0);
        const unsigned winner = calls[0].result == AIRDAP_OTA_STATUS_OK ? 0 : 1;
        assert(calls[winner].result == AIRDAP_OTA_STATUS_OK);
        assert(calls[1 - winner].result == AIRDAP_OTA_STATUS_BUSY);
        assert(begin_calls == 1);
        assert(airdap_ota_disconnect_client(&calls[winner].client) == AIRDAP_OTA_STATUS_OK);
        assert(abort_calls == 1 && !uart_suspended);
    }
}

static void test_failed_disconnect_can_be_cleaned_up_by_new_owner(void)
{
    reset_fakes();
    bool a = true, b = true;
    const airdap_ota_client_t first = {&a, 7, client_authorized}, next = {&b, 7, client_authorized};
    assert(airdap_ota_begin_for_client(&first, 8) == AIRDAP_OTA_STATUS_OK);
    abort_result = ESP_FAIL;
    assert(airdap_ota_disconnect_client(&first) == AIRDAP_OTA_STATUS_INTERNAL_ERROR);
    assert(!airdap_ota_debug_allowed() && uart_suspended);
    assert(airdap_ota_begin_for_client(&next, 8) == AIRDAP_OTA_STATUS_BUSY);
    abort_result = ESP_OK;
    assert(airdap_ota_abort_for_client(&next) == AIRDAP_OTA_STATUS_OK);
    assert(airdap_ota_debug_allowed() && !uart_suspended && activate_calls == 0);
}

int main(void)
{
    const airdap_dap_ownership_backend_t ownership_backend = {
        .line_reset = ownership_line_reset,
        .release_pins = ownership_release_pins,
    };
    airdap_mode_state_init();
    assert(airdap_dap_ownership_initialize(&ownership_backend) ==
        AIRDAP_DAP_OWNERSHIP_OK);

    test_reports_update_capacity_and_running_version();
    test_successful_sequential_update_commits_then_reboots();
    test_rejects_invalid_sizes_offsets_and_arguments();
    test_begin_failure_releases_handle_created_by_idf();
    test_begin_requires_successful_owner_revoke();
    test_partial_commit_can_receive_remaining_bytes();
    test_write_or_validation_failure_never_activates();
    test_abort_and_disconnect_release_only_live_sessions();
    test_abort_failure_keeps_debug_blocked_until_retry_succeeds();
    test_write_abort_failure_keeps_debug_blocked();
    test_pending_image_is_confirmed_only_when_requested();
    test_network_owner_isolated_from_usb_and_other_connections();
    test_network_protocol_and_lifecycle();
    test_revocation_during_commit_cannot_activate();
    test_uart_drain_failure_prevents_flash_begin();
    test_concurrent_clients_cannot_share_flash_handle();
    test_failed_disconnect_can_be_cleaned_up_by_new_owner();
    test_begin_fails_if_physical_release_fails();

    puts("OTA manager tests passed");
    return 0;
}

struct fake_semaphore { pthread_mutex_t mutex; };
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t handle = calloc(1, sizeof(*handle));
    assert(handle && pthread_mutex_init(&handle->mutex, NULL) == 0);
    return handle;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t timeout)
{
    (void) timeout;
    return pthread_mutex_lock(&handle->mutex) == 0 ? pdTRUE : pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    return pthread_mutex_unlock(&handle->mutex) == 0 ? pdTRUE : pdFALSE;
}
esp_err_t airdap_target_uart_suspend(void) { ++uart_suspend_calls; uart_suspended = true; return uart_suspend_result; }
esp_err_t airdap_target_uart_resume(void) { uart_suspended = false; return ESP_OK; }
