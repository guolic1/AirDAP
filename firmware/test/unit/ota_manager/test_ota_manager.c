#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_dap_ownership.h"
#include "airdap_device_identity.h"
#include "airdap_ota.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

enum {
    UPDATE_PARTITION_SIZE = 0x400000,
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
    .address = 0x420000,
    .size = UPDATE_PARTITION_SIZE,
    .type = 0,
    .subtype = 0x11,
    .label = "ota_1",
};
static const airdap_device_identity_t device_identity = {
    .firmware_version = "1.2.3-test",
};

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

static void reset_fakes(void)
{
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
    airdap_ota_initialize();
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
    assert(ownership_line_reset_calls == 1U);
    assert(ownership_release_calls == 1U);
    assert(begin_calls == 1U && begun_size == 100U);
    assert(airdap_ota_write(0U, first, sizeof(first), &next_offset) ==
        AIRDAP_OTA_STATUS_OK);
    assert(next_offset == 60U);
    assert(airdap_ota_write(60U, second, sizeof(second), &next_offset) ==
        AIRDAP_OTA_STATUS_OK);
    assert(next_offset == 100U);
    assert(write_calls == 2U && last_write_size == sizeof(second));

    assert(airdap_ota_commit() == AIRDAP_OTA_STATUS_OK);
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
}

static void test_begin_failure_releases_handle_created_by_idf(void)
{
    reset_fakes();
    begin_result = ESP_FAIL;

    assert(airdap_ota_begin(32U) == AIRDAP_OTA_STATUS_INTERNAL_ERROR);
    assert(begin_calls == 1U);
    assert(abort_calls == 1U && last_handle == UPDATE_HANDLE);
    assert(airdap_ota_debug_allowed());
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
    assert(abort_calls == 1U && activate_calls == 0U);
    assert(airdap_ota_commit() == AIRDAP_OTA_STATUS_INVALID_STATE);

    reset_fakes();
    assert(airdap_ota_begin(sizeof(data)) == AIRDAP_OTA_STATUS_OK);
    assert(airdap_ota_write(0U, data, sizeof(data), &next_offset) ==
        AIRDAP_OTA_STATUS_OK);
    end_result = ESP_ERR_OTA_VALIDATE_FAILED;
    assert(airdap_ota_commit() == AIRDAP_OTA_STATUS_VALIDATION_FAILED);
    assert(activate_calls == 0U);

    reset_fakes();
    assert(airdap_ota_begin(sizeof(data)) == AIRDAP_OTA_STATUS_OK);
    assert(airdap_ota_write(0U, data, sizeof(data), &next_offset) ==
        AIRDAP_OTA_STATUS_OK);
    activate_result = ESP_FAIL;
    assert(airdap_ota_commit() == AIRDAP_OTA_STATUS_ACTIVATION_FAILED);
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
    assert(airdap_ota_commit() == AIRDAP_OTA_STATUS_INVALID_STATE);
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

int main(void)
{
    const airdap_dap_ownership_backend_t ownership_backend = {
        .line_reset = ownership_line_reset,
        .release_pins = ownership_release_pins,
    };
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
    test_pending_image_is_confirmed_only_when_requested();
    test_begin_fails_if_physical_release_fails();

    puts("OTA manager tests passed");
    return 0;
}
