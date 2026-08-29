#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "airdap_usb_descriptors.h"
#include "tinyusb.h"

enum {
    USB_PRODUCT_ID = 0x4021,
    USB_DAP_OUT_ENDPOINT = 0x01,
    USB_DAP_IN_ENDPOINT = 0x81,
    USB_CDC_NOTIFICATION_ENDPOINT = 0x82,
    USB_CDC_OUT_ENDPOINT = 0x03,
    USB_CDC_IN_ENDPOINT = 0x83,
#if CONFIG_AIRDAP_DEBUG_SHELL
    USB_DEBUG_OUT_ENDPOINT = 0x04,
    USB_DEBUG_IN_ENDPOINT = 0x84,
#endif
    USB_FULL_SPEED_MAX_PACKET = 64,
    USB_MS_OS_VENDOR_CODE = 0x20,

    STRING_LANGUAGE = 0,
    STRING_MANUFACTURER = 1,
    STRING_PRODUCT = 2,
    STRING_SERIAL = 3,
    STRING_DAP_INTERFACE = 4,
    STRING_CDC_INTERFACE = 5,
#if CONFIG_AIRDAP_DEBUG_SHELL
    STRING_DEBUG_INTERFACE = 6,
#endif

    CONFIGURATION_TOTAL_LENGTH =
        TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_CDC_DESC_LEN
#if CONFIG_AIRDAP_DEBUG_SHELL
        + TUD_VENDOR_DESC_LEN
#endif
        ,
    MS_OS_20_DAP_FUNCTION_LENGTH = 160,
    MS_OS_20_DEBUG_FUNCTION_LENGTH = 28,
    MS_OS_20_DESCRIPTOR_LENGTH = 178
#if CONFIG_AIRDAP_DEBUG_SHELL
        + MS_OS_20_DEBUG_FUNCTION_LENGTH
#endif
        ,
    MS_OS_20_REG_PROPERTY_DESCRIPTOR_LENGTH = 132,
    MS_OS_20_PROPERTY_NAME_LENGTH = 42,
    MS_OS_20_PROPERTY_DATA_LENGTH = 80,
    MS_OS_20_PROPERTY_DATA_REG_MULTI_SZ = 7,
    BOS_TOTAL_LENGTH = TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN,
};

static const tusb_desc_device_t device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = TINYUSB_ESPRESSIF_VID,
    .idProduct = USB_PRODUCT_ID,
    .bcdDevice = 0x0101,
    .iManufacturer = STRING_MANUFACTURER,
    .iProduct = STRING_PRODUCT,
    .iSerialNumber = STRING_SERIAL,
    .bNumConfigurations = 1,
};

static const uint8_t configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(
        1,
        AIRDAP_USB_INTERFACE_COUNT,
        0,
        CONFIGURATION_TOTAL_LENGTH,
        0,
        100),

    /* CMSIS-DAP v2 requires Bulk OUT to be listed before Bulk IN. */
    TUD_VENDOR_DESCRIPTOR(
        AIRDAP_USB_DAP_INTERFACE,
        STRING_DAP_INTERFACE,
        USB_DAP_OUT_ENDPOINT,
        USB_DAP_IN_ENDPOINT,
        USB_FULL_SPEED_MAX_PACKET),

    TUD_CDC_DESCRIPTOR(
        AIRDAP_USB_CDC_CONTROL_INTERFACE,
        STRING_CDC_INTERFACE,
        USB_CDC_NOTIFICATION_ENDPOINT,
        8,
        USB_CDC_OUT_ENDPOINT,
        USB_CDC_IN_ENDPOINT,
        USB_FULL_SPEED_MAX_PACKET),

#if CONFIG_AIRDAP_DEBUG_SHELL
    TUD_VENDOR_DESCRIPTOR(
        AIRDAP_USB_DEBUG_INTERFACE,
        STRING_DEBUG_INTERFACE,
        USB_DEBUG_OUT_ENDPOINT,
        USB_DEBUG_IN_ENDPOINT,
        USB_FULL_SPEED_MAX_PACKET),
#endif
};

static char usb_serial[17] = "ADP-000000000000";
static const char *string_descriptors[] = {
    (const char[]) {0x09, 0x04},
    "AirDAP",
    "AirDAP CMSIS-DAP v2",
    usb_serial,
    "CMSIS-DAP v2",
    "AirDAP Target UART",
#if CONFIG_AIRDAP_DEBUG_SHELL
    "AirDAP Debug Shell",
#endif
};

static const uint8_t bos_descriptor[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LENGTH, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(
        MS_OS_20_DESCRIPTOR_LENGTH,
        USB_MS_OS_VENDOR_CODE),
};

static const uint8_t ms_os_20_descriptor[] = {
    U16_TO_U8S_LE(0x000A),
    U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000),
    U16_TO_U8S_LE(MS_OS_20_DESCRIPTOR_LENGTH),

    U16_TO_U8S_LE(0x0008),
    U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0x00,
    0x00,
    U16_TO_U8S_LE(MS_OS_20_DESCRIPTOR_LENGTH - 0x0A),

    U16_TO_U8S_LE(0x0008),
    U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    AIRDAP_USB_DAP_INTERFACE,
    0x00,
    U16_TO_U8S_LE(MS_OS_20_DAP_FUNCTION_LENGTH),

    U16_TO_U8S_LE(0x0014),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    /* Keep this interface GUID stable across firmware revisions. */
    U16_TO_U8S_LE(MS_OS_20_REG_PROPERTY_DESCRIPTOR_LENGTH),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(MS_OS_20_PROPERTY_DATA_REG_MULTI_SZ),
    U16_TO_U8S_LE(MS_OS_20_PROPERTY_NAME_LENGTH),
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00,
    'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00,
    't', 0x00, 'e', 0x00, 'r', 0x00, 'f', 0x00,
    'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00,
    'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00,
    0x00, 0x00,
    U16_TO_U8S_LE(MS_OS_20_PROPERTY_DATA_LENGTH),
    '{', 0x00, 'E', 0x00, '0', 0x00, '0', 0x00,
    'E', 0x00, 'C', 0x00, 'B', 0x00, '9', 0x00,
    '8', 0x00, '-', 0x00, 'D', 0x00, 'D', 0x00,
    '2', 0x00, 'B', 0x00, '-', 0x00, '4', 0x00,
    'E', 0x00, '7', 0x00, '0', 0x00, '-', 0x00,
    '8', 0x00, '4', 0x00, '7', 0x00, '1', 0x00,
    '-', 0x00, 'A', 0x00, '7', 0x00, '2', 0x00,
    '2', 0x00, '3', 0x00, 'F', 0x00, 'A', 0x00,
    'D', 0x00, 'D', 0x00, 'A', 0x00, 'F', 0x00,
    '9', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00,

#if CONFIG_AIRDAP_DEBUG_SHELL
    U16_TO_U8S_LE(0x0008),
    U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    AIRDAP_USB_DEBUG_INTERFACE,
    0x00,
    U16_TO_U8S_LE(MS_OS_20_DEBUG_FUNCTION_LENGTH),

    U16_TO_U8S_LE(0x0014),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
#endif
};

_Static_assert(
    sizeof(configuration_descriptor) == CONFIGURATION_TOTAL_LENGTH,
    "USB configuration descriptor length mismatch");
_Static_assert(
    sizeof(bos_descriptor) == BOS_TOTAL_LENGTH,
    "USB BOS descriptor length mismatch");
_Static_assert(
    sizeof(ms_os_20_descriptor) == MS_OS_20_DESCRIPTOR_LENGTH,
    "Microsoft OS 2.0 descriptor length mismatch");

void airdap_usb_descriptors_set_serial(const char *serial_number)
{
    if (serial_number == NULL) {
        return;
    }
    (void) strncpy(usb_serial, serial_number, sizeof(usb_serial) - 1U);
    usb_serial[sizeof(usb_serial) - 1U] = '\0';
}

const tusb_desc_device_t *airdap_usb_device_descriptor(void)
{
    return &device_descriptor;
}

const uint8_t *airdap_usb_configuration_descriptor(void)
{
    return configuration_descriptor;
}

const char **airdap_usb_string_descriptors(void)
{
    return string_descriptors;
}

size_t airdap_usb_string_descriptor_count(void)
{
    return sizeof(string_descriptors) / sizeof(string_descriptors[0]);
}

const uint8_t *tud_descriptor_bos_cb(void)
{
    return bos_descriptor;
}

bool tud_vendor_control_xfer_cb(
    uint8_t rhport,
    uint8_t stage,
    const tusb_control_request_t *request)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (request->bmRequestType_bit.direction != TUSB_DIR_IN ||
        request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR ||
        request->bmRequestType_bit.recipient != TUSB_REQ_RCPT_DEVICE ||
        request->bRequest != USB_MS_OS_VENDOR_CODE ||
        request->wValue != 0U ||
        request->wIndex != 7U) {
        return false;
    }

    const uint16_t length = request->wLength < sizeof(ms_os_20_descriptor)
        ? request->wLength
        : sizeof(ms_os_20_descriptor);
    return tud_control_xfer(
        rhport,
        request,
        (void *) (uintptr_t) ms_os_20_descriptor,
        length);
}
