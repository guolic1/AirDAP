#include <stddef.h>
#include <stdint.h>

#include "airdap_board.h"
#include "airdap_board_pins.h"
#include "driver/gpio.h"

#define AIRDAP_PIN_MASK(pin) (UINT64_C(1) << (pin))

static esp_err_t configure_pins(uint64_t pin_mask, gpio_mode_t mode)
{
    const gpio_config_t config = {
        .pin_bit_mask = pin_mask,
        .mode = mode,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    return gpio_config(&config);
}

static esp_err_t preload_safe_levels(void)
{
    static const struct {
        gpio_num_t pin;
        uint32_t level;
    } levels[] = {
        {(gpio_num_t) AIRDAP_PIN_TARGET_SWCLK_TCK, 0U},
        {(gpio_num_t) AIRDAP_PIN_SWDIO_DIR, 0U},
        {(gpio_num_t) AIRDAP_PIN_TARGET_NRESET, 0U},
        {(gpio_num_t) AIRDAP_PIN_TARGET_TX_TDI, 1U},
        {(gpio_num_t) AIRDAP_PIN_LED_STATUS, 1U},
        {(gpio_num_t) AIRDAP_PIN_LED_NET, 1U},
        {(gpio_num_t) AIRDAP_PIN_V_SOURCE_STATUS, 1U},
    };

    for (size_t index = 0; index < sizeof(levels) / sizeof(levels[0]); ++index) {
        esp_err_t error = gpio_set_level(levels[index].pin, levels[index].level);
        if (error != ESP_OK) {
            return error;
        }
    }

    return ESP_OK;
}

esp_err_t airdap_board_init_safe(void)
{
    const uint64_t input_mask =
        AIRDAP_PIN_MASK(AIRDAP_PIN_BOOT_KEY) |
        AIRDAP_PIN_MASK(AIRDAP_PIN_TARGET_VTREF_ADC) |
        AIRDAP_PIN_MASK(AIRDAP_PIN_USB_VBUS_SENSE) |
        AIRDAP_PIN_MASK(AIRDAP_PIN_TARGET_SWDIO_TMS) |
        AIRDAP_PIN_MASK(AIRDAP_PIN_TARGET_RX_TDO);
    const uint64_t output_mask =
        AIRDAP_PIN_MASK(AIRDAP_PIN_LED_STATUS) |
        AIRDAP_PIN_MASK(AIRDAP_PIN_LED_NET) |
        AIRDAP_PIN_MASK(AIRDAP_PIN_TARGET_SWCLK_TCK) |
        AIRDAP_PIN_MASK(AIRDAP_PIN_SWDIO_DIR) |
        AIRDAP_PIN_MASK(AIRDAP_PIN_TARGET_TX_TDI) |
        AIRDAP_PIN_MASK(AIRDAP_PIN_TARGET_NRESET);
    esp_err_t error = preload_safe_levels();

    if (error != ESP_OK) {
        return error;
    }

    error = configure_pins(input_mask, GPIO_MODE_INPUT);
    if (error != ESP_OK) {
        return error;
    }

    error = configure_pins(output_mask, GPIO_MODE_OUTPUT);
    if (error != ESP_OK) {
        return error;
    }

    return configure_pins(
        AIRDAP_PIN_MASK(AIRDAP_PIN_V_SOURCE_STATUS),
        GPIO_MODE_INPUT_OUTPUT_OD);
}

esp_err_t airdap_target_power_set_allowed(bool allowed)
{
    return gpio_set_level(
        (gpio_num_t) AIRDAP_PIN_V_SOURCE_STATUS,
        allowed ? 1U : 0U);
}

esp_err_t airdap_target_power_get_active(bool *active)
{
    if (active == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *active = gpio_get_level((gpio_num_t) AIRDAP_PIN_V_SOURCE_STATUS) != 0;
    return ESP_OK;
}

esp_err_t airdap_target_reset_set_asserted(bool asserted)
{
    return gpio_set_level(
        (gpio_num_t) AIRDAP_PIN_TARGET_NRESET,
        asserted ? 1U : 0U);
}

esp_err_t airdap_boot_key_get_pressed(bool *pressed)
{
    if (pressed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *pressed = gpio_get_level((gpio_num_t) AIRDAP_PIN_BOOT_KEY) == 0;
    return ESP_OK;
}
