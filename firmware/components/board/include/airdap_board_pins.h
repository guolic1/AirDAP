#pragma once

/*
 * AirDAP ESP32-S3 pin assignment.
 *
 * This header intentionally has no ESP-IDF dependencies so the application,
 * bootloader, and host tests all consume the same hardware contract.
 */
enum {
    AIRDAP_PIN_BOOT_KEY = 0,
    AIRDAP_PIN_TARGET_VTREF_ADC = 3,
    AIRDAP_PIN_USB_VBUS_SENSE = 8,
    AIRDAP_PIN_V_SOURCE_STATUS = 9,
    AIRDAP_PIN_LED_STATUS = 10,
    AIRDAP_PIN_LED_NET = 11,
    AIRDAP_PIN_TARGET_SWCLK_TCK = 12,
    AIRDAP_PIN_TARGET_SWDIO_TMS = 13,
    AIRDAP_PIN_SWDIO_DIR = 14,
    AIRDAP_PIN_TARGET_TX_TDI = 17,
    AIRDAP_PIN_TARGET_RX_TDO = 18,
    AIRDAP_PIN_USB_DM = 19,
    AIRDAP_PIN_USB_DP = 20,
    AIRDAP_PIN_TARGET_NRESET = 41,
};
