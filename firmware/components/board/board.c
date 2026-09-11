#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#include "airdap_board.h"
#include "airdap_board_pins.h"
#include "driver/gpio.h"

#define AIRDAP_PIN_MASK(pin) (UINT64_C(1) << (pin))

static atomic_bool boot_key_simulated_pressed;

static esp_err_t configure_pins(
    uint64_t pin_mask,
    gpio_mode_t mode,
    gpio_pullup_t pull_up)
{
    const gpio_config_t config = {
        .pin_bit_mask = pin_mask,
        .mode = mode,
        .pull_up_en = pull_up,
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

    error = configure_pins(
        AIRDAP_PIN_MASK(AIRDAP_PIN_BOOT_KEY),
        GPIO_MODE_INPUT,
        GPIO_PULLUP_ENABLE);
    if (error != ESP_OK) {
        return error;
    }

    error = configure_pins(
        input_mask,
        GPIO_MODE_INPUT,
        GPIO_PULLUP_DISABLE);
    if (error != ESP_OK) {
        return error;
    }

    error = configure_pins(
        output_mask,
        GPIO_MODE_OUTPUT,
        GPIO_PULLUP_DISABLE);
    if (error != ESP_OK) {
        return error;
    }

    return configure_pins(
        AIRDAP_PIN_MASK(AIRDAP_PIN_V_SOURCE_STATUS),
        GPIO_MODE_INPUT_OUTPUT_OD,
        GPIO_PULLUP_DISABLE);
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
    *pressed = gpio_get_level((gpio_num_t) AIRDAP_PIN_BOOT_KEY) == 0 ||
        atomic_load(&boot_key_simulated_pressed);
    return ESP_OK;
}

esp_err_t airdap_boot_key_set_simulated_pressed(bool pressed)
{
    atomic_store(&boot_key_simulated_pressed, pressed);
    return ESP_OK;
}

esp_err_t airdap_boot_key_get_simulated_pressed(bool *pressed)
{
    if (pressed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *pressed = atomic_load(&boot_key_simulated_pressed);
    return ESP_OK;
}

esp_err_t airdap_board_leds_set(bool status_on, bool network_on)
{
    const esp_err_t error = gpio_set_level(
        (gpio_num_t) AIRDAP_PIN_LED_STATUS,
        status_on ? 0U : 1U);
    return error == ESP_OK
        ? gpio_set_level(
            (gpio_num_t) AIRDAP_PIN_LED_NET,
            network_on ? 0U : 1U)
        : error;
}
