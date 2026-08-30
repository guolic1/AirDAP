#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "airdap_debug_shell_identity.h"

static bool append_formatted(
    char *output,
    size_t output_size,
    size_t *offset,
    const char *format,
    ...)
{
    if (*offset >= output_size) {
        return false;
    }

    va_list arguments;
    va_start(arguments, format);
    const int formatted = vsnprintf(
        output + *offset,
        output_size - *offset,
        format,
        arguments);
    va_end(arguments);
    if (formatted < 0 || (size_t) formatted >= output_size - *offset) {
        return false;
    }

    *offset += (size_t) formatted;
    return true;
}

bool airdap_debug_shell_identity_format(
    const airdap_device_identity_t *identity,
    char *output,
    size_t output_size)
{
    if (identity == NULL || identity->firmware_version == NULL ||
        output == NULL || output_size == 0U) {
        return false;
    }

    size_t offset = 0U;
    if (!append_formatted(
            output,
            output_size,
            &offset,
            "usb_serial=%s\ndevice_id=%s\nuuid=",
            identity->usb_serial,
            identity->device_id)) {
        return false;
    }
    for (size_t index = 0U; index < AIRDAP_DEVICE_UUID_SIZE; ++index) {
        if (!append_formatted(
                output,
                output_size,
                &offset,
                "%02X",
                identity->uuid[index])) {
            return false;
        }
    }
    return append_formatted(
        output,
        output_size,
        &offset,
        "\nfirmware_version=%s\nprotocol_version=%" PRIu8
        "\ncapabilities=0x%08" PRIX32 "\n",
        identity->firmware_version,
        identity->protocol_version,
        identity->capabilities);
}
