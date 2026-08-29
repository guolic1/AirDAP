#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "airdap_usb_descriptors.h"

enum {
    EXPECTED_CONFIGURATION_LENGTH = 98,
    EXPECTED_BOS_LENGTH = 33,
    EXPECTED_MS_OS_20_LENGTH = 46,
    EXPECTED_MS_VENDOR_CODE = 0x20,
};

static bool control_called;
static uint8_t control_rhport;
static const tusb_control_request_t *control_request;
static const uint8_t *control_data;
static uint16_t control_length;

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t) data[0] | ((uint16_t) data[1] << 8U);
}

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t) data[0] |
        ((uint32_t) data[1] << 8U) |
        ((uint32_t) data[2] << 16U) |
        ((uint32_t) data[3] << 24U);
}

bool tud_control_xfer(
    uint8_t rhport,
    const tusb_control_request_t *request,
    void *buffer,
    uint16_t length)
{
    control_called = true;
    control_rhport = rhport;
    control_request = request;
    control_data = buffer;
    control_length = length;
    return true;
}

static void reset_control_capture(void)
{
    control_called = false;
    control_rhport = 0U;
    control_request = NULL;
    control_data = NULL;
    control_length = 0U;
}

static void test_device_descriptor(void)
{
    const tusb_desc_device_t *descriptor = airdap_usb_device_descriptor();

    assert(descriptor != NULL);
    assert(descriptor->bLength == sizeof(*descriptor));
    assert(descriptor->bDescriptorType == TUSB_DESC_DEVICE);
    assert(descriptor->bcdUSB == 0x0210U);
    assert(descriptor->bDeviceClass == TUSB_CLASS_MISC);
    assert(descriptor->bDeviceSubClass == MISC_SUBCLASS_COMMON);
    assert(descriptor->bDeviceProtocol == MISC_PROTOCOL_IAD);
    assert(descriptor->bMaxPacketSize0 == 64U);
    assert(descriptor->idVendor == 0x303AU);
    assert(descriptor->idProduct == 0x4021U);
    assert(descriptor->bcdDevice == 0x0100U);
    assert(descriptor->iManufacturer == 1U);
    assert(descriptor->iProduct == 2U);
    assert(descriptor->iSerialNumber == 3U);
    assert(descriptor->bNumConfigurations == 1U);
}

static void test_configuration_descriptor(void)
{
    const uint8_t *descriptor = airdap_usb_configuration_descriptor();
    static const uint8_t expected_endpoints[] = {0x01, 0x81, 0x82, 0x03, 0x83};
    static const uint8_t expected_attributes[] = {
        TUSB_XFER_BULK,
        TUSB_XFER_BULK,
        TUSB_XFER_INTERRUPT,
        TUSB_XFER_BULK,
        TUSB_XFER_BULK,
    };
    static const uint16_t expected_packet_sizes[] = {64, 64, 8, 64, 64};
    size_t offset = 0U;
    size_t endpoint_count = 0U;
    unsigned int interface_mask = 0U;
    bool found_iad = false;

    assert(descriptor != NULL);
    assert(descriptor[0] == 9U);
    assert(descriptor[1] == TUSB_DESC_CONFIGURATION);
    assert(read_u16(descriptor + 2U) == EXPECTED_CONFIGURATION_LENGTH);
    assert(descriptor[4] == AIRDAP_USB_INTERFACE_COUNT);
    assert(descriptor[5] == 1U);
    assert(descriptor[7] == 0x80U);
    assert(descriptor[8] == 50U);

    while (offset < EXPECTED_CONFIGURATION_LENGTH) {
        const uint8_t length = descriptor[offset];
        const uint8_t type = descriptor[offset + 1U];
        assert(length >= 2U);
        assert(offset + length <= EXPECTED_CONFIGURATION_LENGTH);

        if (type == TUSB_DESC_INTERFACE) {
            const uint8_t number = descriptor[offset + 2U];
            assert(number < AIRDAP_USB_INTERFACE_COUNT);
            interface_mask |= 1U << number;
            if (number == AIRDAP_USB_DAP_INTERFACE) {
                assert(descriptor[offset + 4U] == 2U);
                assert(descriptor[offset + 5U] == TUSB_CLASS_VENDOR_SPECIFIC);
                assert(descriptor[offset + 6U] == 0U);
                assert(descriptor[offset + 7U] == 0U);
                assert(descriptor[offset + 8U] == 4U);
            } else if (number == AIRDAP_USB_CDC_CONTROL_INTERFACE) {
                assert(descriptor[offset + 5U] == TUSB_CLASS_CDC);
                assert(descriptor[offset + 8U] == 5U);
            } else {
                assert(number == AIRDAP_USB_CDC_DATA_INTERFACE);
                assert(descriptor[offset + 5U] == TUSB_CLASS_CDC_DATA);
            }
        } else if (type == TUSB_DESC_ENDPOINT) {
            assert(endpoint_count < sizeof(expected_endpoints));
            assert(descriptor[offset + 2U] == expected_endpoints[endpoint_count]);
            assert((descriptor[offset + 3U] & 0x03U) ==
                expected_attributes[endpoint_count]);
            assert(read_u16(descriptor + offset + 4U) ==
                expected_packet_sizes[endpoint_count]);
            ++endpoint_count;
        } else if (type == TUSB_DESC_INTERFACE_ASSOCIATION) {
            assert(descriptor[offset + 2U] == AIRDAP_USB_CDC_CONTROL_INTERFACE);
            assert(descriptor[offset + 3U] == 2U);
            found_iad = true;
        }
        offset += length;
    }

    assert(offset == EXPECTED_CONFIGURATION_LENGTH);
    assert(interface_mask == 0x07U);
    assert(endpoint_count == sizeof(expected_endpoints));
    assert(found_iad);
}

static void test_strings_and_stable_serial(void)
{
    const char **strings = airdap_usb_string_descriptors();

    assert(airdap_usb_string_descriptor_count() == 6U);
    assert((uint8_t) strings[0][0] == 0x09U);
    assert((uint8_t) strings[0][1] == 0x04U);
    assert(strcmp(strings[1], "AirDAP") == 0);
    assert(strstr(strings[2], "CMSIS-DAP") != NULL);
    assert(strstr(strings[4], "CMSIS-DAP") != NULL);
    assert(strstr(strings[5], "UART") != NULL);

    airdap_usb_descriptors_set_serial("ADP-A1B2C3D4E5F6");
    assert(strcmp(strings[3], "ADP-A1B2C3D4E5F6") == 0);
    airdap_usb_descriptors_set_serial("ADP-001122334455-extra-data");
    assert(strcmp(strings[3], "ADP-001122334455") == 0);
    airdap_usb_descriptors_set_serial(NULL);
    assert(strcmp(strings[3], "ADP-001122334455") == 0);
}

static void test_bos_descriptor(void)
{
    static const uint8_t ms_os_uuid[] = {
        0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
        0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
    };
    const uint8_t *descriptor = tud_descriptor_bos_cb();

    assert(descriptor != NULL);
    assert(descriptor[0] == 5U && descriptor[1] == TUSB_DESC_BOS);
    assert(read_u16(descriptor + 2U) == EXPECTED_BOS_LENGTH);
    assert(descriptor[4] == 1U);
    assert(descriptor[5] == 28U);
    assert(descriptor[6] == TUSB_DESC_DEVICE_CAPABILITY);
    assert(memcmp(descriptor + 9U, ms_os_uuid, sizeof(ms_os_uuid)) == 0);
    assert(read_u32(descriptor + 25U) == 0x06030000U);
    assert(read_u16(descriptor + 29U) == EXPECTED_MS_OS_20_LENGTH);
    assert(descriptor[31] == EXPECTED_MS_VENDOR_CODE);
    assert(descriptor[32] == 0U);
}

static tusb_control_request_t make_ms_request(void)
{
    tusb_control_request_t request = {0};
    request.bmRequestType_bit.direction = TUSB_DIR_IN;
    request.bmRequestType_bit.type = TUSB_REQ_TYPE_VENDOR;
    request.bmRequestType_bit.recipient = TUSB_REQ_RCPT_DEVICE;
    request.bRequest = EXPECTED_MS_VENDOR_CODE;
    request.wIndex = 7U;
    request.wLength = UINT16_MAX;
    return request;
}

static void assert_ms_os_20_descriptor(const uint8_t *descriptor)
{
    assert(descriptor != NULL);
    assert(read_u16(descriptor) == 10U);
    assert(read_u16(descriptor + 2U) == MS_OS_20_SET_HEADER_DESCRIPTOR);
    assert(read_u32(descriptor + 4U) == 0x06030000U);
    assert(read_u16(descriptor + 8U) == EXPECTED_MS_OS_20_LENGTH);

    assert(read_u16(descriptor + 10U) == 8U);
    assert(read_u16(descriptor + 12U) == MS_OS_20_SUBSET_HEADER_CONFIGURATION);
    assert(descriptor[14] == 0U);
    assert(read_u16(descriptor + 16U) == 36U);

    assert(read_u16(descriptor + 18U) == 8U);
    assert(read_u16(descriptor + 20U) == MS_OS_20_SUBSET_HEADER_FUNCTION);
    assert(descriptor[22] == AIRDAP_USB_DAP_INTERFACE);
    assert(read_u16(descriptor + 24U) == 28U);

    assert(read_u16(descriptor + 26U) == 20U);
    assert(read_u16(descriptor + 28U) == MS_OS_20_FEATURE_COMPATBLE_ID);
    assert(memcmp(descriptor + 30U, "WINUSB\0\0", 8U) == 0);
}

static void test_ms_os_20_control_request(void)
{
    tusb_control_request_t request = make_ms_request();

    reset_control_capture();
    assert(tud_vendor_control_xfer_cb(2U, CONTROL_STAGE_SETUP, &request));
    assert(control_called);
    assert(control_rhport == 2U);
    assert(control_request == &request);
    assert(control_length == EXPECTED_MS_OS_20_LENGTH);
    assert_ms_os_20_descriptor(control_data);

    request.wLength = 16U;
    reset_control_capture();
    assert(tud_vendor_control_xfer_cb(1U, CONTROL_STAGE_SETUP, &request));
    assert(control_called && control_length == 16U);

    request = make_ms_request();
    request.bmRequestType_bit.direction = TUSB_DIR_OUT;
    reset_control_capture();
    assert(!tud_vendor_control_xfer_cb(0U, CONTROL_STAGE_SETUP, &request));
    assert(!control_called);

    request = make_ms_request();
    request.bmRequestType_bit.recipient = TUSB_REQ_RCPT_INTERFACE;
    reset_control_capture();
    assert(!tud_vendor_control_xfer_cb(0U, CONTROL_STAGE_SETUP, &request));
    assert(!control_called);

    request = make_ms_request();
    request.wValue = 1U;
    reset_control_capture();
    assert(!tud_vendor_control_xfer_cb(0U, CONTROL_STAGE_SETUP, &request));
    assert(!control_called);

    request = make_ms_request();
    reset_control_capture();
    assert(tud_vendor_control_xfer_cb(0U, CONTROL_STAGE_DATA, &request));
    assert(!control_called);
}

int main(void)
{
    test_device_descriptor();
    test_configuration_descriptor();
    test_strings_and_stable_serial();
    test_bos_descriptor();
    test_ms_os_20_control_request();

    puts("USB descriptor tests passed");
    return 0;
}
