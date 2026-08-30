#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_dap_ota.h"
#include "airdap_ota.h"

enum {
    OTA_QUERY = 0x80,
    OTA_BEGIN = 0x81,
    OTA_WRITE = 0x82,
    OTA_COMMIT = 0x83,
    OTA_ABORT = 0x84,
    OTA_REBOOT = 0x85,
};

static airdap_ota_status_t info_status;
static airdap_ota_status_t begin_status;
static airdap_ota_status_t write_status;
static airdap_ota_status_t commit_status;
static airdap_ota_status_t abort_status;
static airdap_ota_status_t reboot_status;
static uint32_t begun_size;
static uint32_t written_offset;
static size_t written_size;
static uint8_t written_data[496];
static unsigned begin_calls;
static unsigned write_calls;
static unsigned commit_calls;
static unsigned abort_calls;
static unsigned reboot_calls;

static void reset_fakes(void)
{
    info_status = AIRDAP_OTA_STATUS_OK;
    begin_status = AIRDAP_OTA_STATUS_OK;
    write_status = AIRDAP_OTA_STATUS_OK;
    commit_status = AIRDAP_OTA_STATUS_OK;
    abort_status = AIRDAP_OTA_STATUS_OK;
    reboot_status = AIRDAP_OTA_STATUS_OK;
    begun_size = 0U;
    written_offset = 0U;
    written_size = 0U;
    memset(written_data, 0, sizeof(written_data));
    begin_calls = 0U;
    write_calls = 0U;
    commit_calls = 0U;
    abort_calls = 0U;
    reboot_calls = 0U;
}

airdap_ota_status_t airdap_ota_get_info(airdap_ota_info_t *info)
{
    assert(info != NULL);
    if (info_status == AIRDAP_OTA_STATUS_OK) {
        memset(info, 0, sizeof(*info));
        info->protocol_version = AIRDAP_OTA_PROTOCOL_VERSION;
        info->flags = AIRDAP_OTA_FLAG_ROLLBACK;
        info->max_image_size = 0x400000U;
        memcpy(info->running_version, "2.3.4-dev", sizeof("2.3.4-dev"));
    }
    return info_status;
}

airdap_ota_status_t airdap_ota_begin(uint32_t image_size)
{
    ++begin_calls;
    begun_size = image_size;
    return begin_status;
}

airdap_ota_status_t airdap_ota_write(
    uint32_t offset,
    const void *data,
    size_t size,
    uint32_t *next_offset)
{
    assert(data != NULL && next_offset != NULL && size <= sizeof(written_data));
    ++write_calls;
    written_offset = offset;
    written_size = size;
    memcpy(written_data, data, size);
    *next_offset = offset + (uint32_t) size;
    return write_status;
}

airdap_ota_status_t airdap_ota_commit(void)
{
    ++commit_calls;
    return commit_status;
}

airdap_ota_status_t airdap_ota_abort(void)
{
    ++abort_calls;
    return abort_status;
}

airdap_ota_status_t airdap_ota_reboot(void)
{
    ++reboot_calls;
    return reboot_status;
}

static size_t process(
    bool debug_connected,
    const uint8_t *request,
    size_t request_length,
    uint8_t *response,
    size_t response_capacity)
{
    memset(response, 0xCC, response_capacity);
    return airdap_dap_ota_process(
        debug_connected,
        request,
        request_length,
        response,
        response_capacity);
}

static void test_query_serializes_capabilities_and_version(void)
{
    uint8_t response[64];
    const uint8_t request[] = {OTA_QUERY};

    reset_fakes();
    const size_t length = process(
        false, request, sizeof(request), response, sizeof(response));
    assert(length == 9U + strlen("2.3.4-dev"));
    assert(response[0] == OTA_QUERY && response[1] == AIRDAP_OTA_STATUS_OK);
    assert(response[2] == AIRDAP_OTA_PROTOCOL_VERSION);
    assert(response[3] == AIRDAP_OTA_FLAG_ROLLBACK);
    assert(response[4] == 0x00U && response[5] == 0x00U &&
        response[6] == 0x40U && response[7] == 0x00U);
    assert(response[8] == strlen("2.3.4-dev"));
    assert(memcmp(response + 9U, "2.3.4-dev", response[8]) == 0);

    info_status = AIRDAP_OTA_STATUS_INTERNAL_ERROR;
    assert(process(false, request, sizeof(request), response, sizeof(response)) == 2U);
    assert(response[1] == AIRDAP_OTA_STATUS_INTERNAL_ERROR);
    info_status = AIRDAP_OTA_STATUS_OK;
    assert(process(false, request, sizeof(request), response, 8U) == 0U);
}

static void test_begin_requires_disconnected_debug_port(void)
{
    uint8_t response[16];
    const uint8_t request[] = {OTA_BEGIN, 0x78, 0x56, 0x34, 0x12};

    reset_fakes();
    assert(process(true, request, sizeof(request), response, sizeof(response)) == 2U);
    assert(response[0] == OTA_BEGIN &&
        response[1] == AIRDAP_OTA_STATUS_INVALID_STATE);
    assert(begin_calls == 0U);

    assert(process(false, request, sizeof(request), response, sizeof(response)) == 2U);
    assert(response[1] == AIRDAP_OTA_STATUS_OK);
    assert(begin_calls == 1U && begun_size == 0x12345678U);
}

static void test_write_parses_payload_and_returns_next_offset(void)
{
    uint8_t response[16];
    const uint8_t request[] = {
        OTA_WRITE,
        0x04, 0x03, 0x02, 0x01,
        3, 0,
        0xAA, 0xBB, 0xCC,
    };

    reset_fakes();
    assert(process(false, request, sizeof(request), response, sizeof(response)) == 6U);
    assert(response[0] == OTA_WRITE && response[1] == AIRDAP_OTA_STATUS_OK);
    assert(response[2] == 0x07U && response[3] == 0x03U &&
        response[4] == 0x02U && response[5] == 0x01U);
    assert(write_calls == 1U && written_offset == 0x01020304U);
    assert(written_size == 3U && memcmp(written_data, request + 7U, 3U) == 0);

    write_status = AIRDAP_OTA_STATUS_INVALID_OFFSET;
    assert(process(false, request, sizeof(request), response, sizeof(response)) == 2U);
    assert(response[1] == AIRDAP_OTA_STATUS_INVALID_OFFSET);

    assert(process(true, request, sizeof(request), response, sizeof(response)) == 2U);
    assert(response[1] == AIRDAP_OTA_STATUS_INVALID_STATE);
}

static void test_fixed_commands_and_malformed_requests(void)
{
    uint8_t response[16];

    reset_fakes();
    const uint8_t commit[] = {OTA_COMMIT};
    assert(process(false, commit, sizeof(commit), response, sizeof(response)) == 2U);
    assert(response[1] == AIRDAP_OTA_STATUS_OK && commit_calls == 1U);

    const uint8_t abort_request[] = {OTA_ABORT};
    abort_status = AIRDAP_OTA_STATUS_INTERNAL_ERROR;
    assert(process(
        false, abort_request, sizeof(abort_request), response, sizeof(response)) == 2U);
    assert(response[1] == AIRDAP_OTA_STATUS_INTERNAL_ERROR && abort_calls == 1U);

    const uint8_t reboot[] = {OTA_REBOOT};
    assert(process(false, reboot, sizeof(reboot), response, sizeof(response)) == 0U);
    assert(reboot_calls == 1U);
    reboot_status = AIRDAP_OTA_STATUS_INVALID_STATE;
    assert(process(false, reboot, sizeof(reboot), response, sizeof(response)) == 2U);
    assert(response[1] == AIRDAP_OTA_STATUS_INVALID_STATE && reboot_calls == 2U);

    const uint8_t short_begin[] = {OTA_BEGIN};
    assert(process(false, short_begin, sizeof(short_begin), response, sizeof(response)) == 2U);
    assert(response[1] == AIRDAP_OTA_STATUS_INVALID_ARGUMENT);

    const uint8_t bad_write[] = {OTA_WRITE, 0, 0, 0, 0, 2, 0, 0xAA};
    assert(process(false, bad_write, sizeof(bad_write), response, sizeof(response)) == 2U);
    assert(response[1] == AIRDAP_OTA_STATUS_INVALID_ARGUMENT);

    const uint8_t unknown[] = {0x86};
    assert(process(false, unknown, sizeof(unknown), response, sizeof(response)) == 0U);
}

int main(void)
{
    test_query_serializes_capabilities_and_version();
    test_begin_requires_disconnected_debug_port();
    test_write_parses_payload_and_returns_next_offset();
    test_fixed_commands_and_malformed_requests();
    puts("DAP OTA command tests passed");
    return 0;
}
