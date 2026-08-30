#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_debug_shell_identity.h"

static void test_formats_complete_identity(void)
{
    static const char expected[] =
        "usb_serial=ADP-001122334455\n"
        "device_id=DEV-001122334455\n"
        "uuid=000102030405060708090A0B0C0D0E0F\n"
        "firmware_version=0a6f751\n"
        "protocol_version=7\n"
        "capabilities=0xA5A5001F\n";
    const airdap_device_identity_t identity = {
        .usb_serial = "ADP-001122334455",
        .device_id = "DEV-001122334455",
        .uuid = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        },
        .firmware_version = "0a6f751",
        .protocol_version = 7,
        .capabilities = UINT32_C(0xA5A5001F),
    };
    char output[sizeof(expected)];

    assert(airdap_debug_shell_identity_format(
        &identity,
        output,
        sizeof(output)));
    assert(strcmp(output, expected) == 0);

    char short_output[sizeof(expected) - 1U];
    assert(!airdap_debug_shell_identity_format(
        &identity,
        short_output,
        sizeof(short_output)));
    assert(AIRDAP_DEBUG_SHELL_IDENTITY_OUTPUT_SIZE >= sizeof(expected));
}

static void test_rejects_invalid_arguments_and_small_output(void)
{
    const airdap_device_identity_t identity = {
        .usb_serial = "ADP-001122334455",
        .device_id = "ADP-001122334455",
        .firmware_version = "version",
    };
    char output[32];

    assert(!airdap_debug_shell_identity_format(NULL, output, sizeof(output)));
    assert(!airdap_debug_shell_identity_format(&identity, NULL, sizeof(output)));
    assert(!airdap_debug_shell_identity_format(&identity, output, 0U));
    assert(!airdap_debug_shell_identity_format(&identity, output, sizeof(output)));
}

int main(void)
{
    test_formats_complete_identity();
    test_rejects_invalid_arguments_and_small_output();

    puts("Debug shell identity tests passed");
    return 0;
}
