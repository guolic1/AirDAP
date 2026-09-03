#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "airdap_board.h"
#include "airdap_board_pins.h"

_Static_assert(AIRDAP_PIN_BOOT_KEY == 0, "BOOT_KEY pin must match the hardware pin map");
_Static_assert(AIRDAP_PIN_TARGET_VTREF_ADC == 3, "VTref ADC pin must match the hardware pin map");
_Static_assert(AIRDAP_PIN_USB_VBUS_SENSE == 8, "USB VBUS sense pin must match the hardware pin map");
_Static_assert(AIRDAP_PIN_V_SOURCE_STATUS == 9, "V_SOURCE_STATUS pin must match the hardware pin map");
_Static_assert(AIRDAP_PIN_LED_STATUS == 10, "status LED pin must match the hardware pin map");
_Static_assert(AIRDAP_PIN_LED_NET == 11, "network LED pin must match the hardware pin map");
_Static_assert(AIRDAP_PIN_TARGET_SWCLK_TCK == 12, "SWCLK pin must match the hardware pin map");
_Static_assert(AIRDAP_PIN_TARGET_SWDIO_TMS == 13, "SWDIO pin must match the hardware pin map");
_Static_assert(AIRDAP_PIN_SWDIO_DIR == 14, "SWDIO direction pin must match the hardware pin map");
_Static_assert(AIRDAP_PIN_TARGET_TX_TDI == 17, "target TX pin must match the hardware pin map");
_Static_assert(AIRDAP_PIN_TARGET_RX_TDO == 18, "target RX pin must match the hardware pin map");
_Static_assert(AIRDAP_PIN_USB_DM == 19, "USB D- pin must match the hardware pin map");
_Static_assert(AIRDAP_PIN_USB_DP == 20, "USB D+ pin must match the hardware pin map");
_Static_assert(AIRDAP_PIN_TARGET_NRESET == 41, "target reset pin must match the hardware pin map");
#include "driver/gpio.h"

enum {
    EVENT_CAPACITY = 16,
};

typedef enum {
    EVENT_SET_LEVEL,
    EVENT_CONFIG,
} event_type_t;

typedef struct {
    event_type_t type;
    gpio_num_t pin;
    uint32_t level;
    gpio_config_t config;
} event_t;

static event_t events[EVENT_CAPACITY];
static size_t event_count;
static size_t fail_at_call;
static int input_level;
static gpio_num_t input_pin;

static uint64_t pin_mask(unsigned int pin)
{
    return UINT64_C(1) << pin;
}

static void reset_fake_gpio(void)
{
    event_count = 0;
    fail_at_call = SIZE_MAX;
    input_level = 0;
    input_pin = AIRDAP_PIN_V_SOURCE_STATUS;
}

static esp_err_t record_event(event_t event)
{
    assert(event_count < EVENT_CAPACITY);
    events[event_count] = event;

    if (event_count++ == fail_at_call) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    return record_event((event_t) {
        .type = EVENT_SET_LEVEL,
        .pin = gpio_num,
        .level = level,
    });
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(config != NULL);
    return record_event((event_t) {
        .type = EVENT_CONFIG,
        .config = *config,
    });
}

int gpio_get_level(gpio_num_t gpio_num)
{
    assert(gpio_num == input_pin);
    return input_level;
}

static void test_boot_key_uses_active_low_level(void)
{
    bool pressed = false;
    reset_fake_gpio();
    input_pin = AIRDAP_PIN_BOOT_KEY;

    input_level = 0;
    assert(airdap_boot_key_get_pressed(&pressed) == ESP_OK);
    assert(pressed);
    input_level = 1;
    assert(airdap_boot_key_get_pressed(&pressed) == ESP_OK);
    assert(!pressed);
    assert(airdap_boot_key_get_pressed(NULL) == ESP_ERR_INVALID_ARG);
}

static const event_t *find_level_event(gpio_num_t pin)
{
    for (size_t index = 0; index < event_count; ++index) {
        if (events[index].type == EVENT_SET_LEVEL && events[index].pin == pin) {
            return &events[index];
        }
    }

    assert(false);
    return NULL;
}

static size_t find_level_event_index(gpio_num_t pin)
{
    return (size_t) (find_level_event(pin) - events);
}

static const event_t *find_config_event(gpio_mode_t mode)
{
    for (size_t index = 0; index < event_count; ++index) {
        if (events[index].type == EVENT_CONFIG && events[index].config.mode == mode) {
            return &events[index];
        }
    }

    assert(false);
    return NULL;
}

static size_t find_config_event_index(gpio_mode_t mode)
{
    return (size_t) (find_config_event(mode) - events);
}

static void assert_config(
    gpio_mode_t mode,
    uint64_t expected_mask,
    gpio_pullup_t expected_pullup,
    gpio_pulldown_t expected_pulldown)
{
    const gpio_config_t *config = &find_config_event(mode)->config;

    assert(config->pin_bit_mask == expected_mask);
    assert(config->pull_up_en == expected_pullup);
    assert(config->pull_down_en == expected_pulldown);
    assert(config->intr_type == GPIO_INTR_DISABLE);
}

static void assert_level_preloaded(gpio_num_t pin, uint32_t level, gpio_mode_t mode)
{
    assert(find_level_event(pin)->level == level);
    assert(find_level_event_index(pin) < find_config_event_index(mode));
}

static void test_safe_gpio_state(void)
{
    const uint64_t input_mask =
        pin_mask(AIRDAP_PIN_BOOT_KEY) |
        pin_mask(AIRDAP_PIN_TARGET_VTREF_ADC) |
        pin_mask(AIRDAP_PIN_USB_VBUS_SENSE) |
        pin_mask(AIRDAP_PIN_TARGET_SWDIO_TMS) |
        pin_mask(AIRDAP_PIN_TARGET_RX_TDO);
    const uint64_t output_mask =
        pin_mask(AIRDAP_PIN_LED_STATUS) |
        pin_mask(AIRDAP_PIN_LED_NET) |
        pin_mask(AIRDAP_PIN_TARGET_SWCLK_TCK) |
        pin_mask(AIRDAP_PIN_SWDIO_DIR) |
        pin_mask(AIRDAP_PIN_TARGET_TX_TDI) |
        pin_mask(AIRDAP_PIN_TARGET_NRESET);

    reset_fake_gpio();

    assert(airdap_board_init_safe() == ESP_OK);
    assert(event_count == 10U);

    assert_config(GPIO_MODE_INPUT, input_mask, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE);
    assert_config(GPIO_MODE_OUTPUT, output_mask, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE);
    assert_config(
        GPIO_MODE_INPUT_OUTPUT_OD,
        pin_mask(AIRDAP_PIN_V_SOURCE_STATUS),
        GPIO_PULLUP_DISABLE,
        GPIO_PULLDOWN_DISABLE);

    assert_level_preloaded(AIRDAP_PIN_TARGET_SWCLK_TCK, 0U, GPIO_MODE_OUTPUT);
    assert_level_preloaded(AIRDAP_PIN_SWDIO_DIR, 0U, GPIO_MODE_OUTPUT);
    assert_level_preloaded(AIRDAP_PIN_TARGET_NRESET, 0U, GPIO_MODE_OUTPUT);
    assert_level_preloaded(AIRDAP_PIN_TARGET_TX_TDI, 1U, GPIO_MODE_OUTPUT);
    assert_level_preloaded(AIRDAP_PIN_LED_STATUS, 1U, GPIO_MODE_OUTPUT);
    assert_level_preloaded(AIRDAP_PIN_LED_NET, 1U, GPIO_MODE_OUTPUT);
    assert_level_preloaded(AIRDAP_PIN_V_SOURCE_STATUS, 1U, GPIO_MODE_INPUT_OUTPUT_OD);
}

static void test_first_gpio_failure_is_returned(void)
{
    reset_fake_gpio();
    fail_at_call = 0U;

    assert(airdap_board_init_safe() == ESP_FAIL);
    assert(event_count == 1U);
}

static void test_gpio_config_failure_is_returned(void)
{
    reset_fake_gpio();
    fail_at_call = 7U;

    assert(airdap_board_init_safe() == ESP_FAIL);
    assert(event_count == 8U);
}

static void test_target_power_control_uses_open_drain_release_levels(void)
{
    reset_fake_gpio();

    assert(airdap_target_power_set_allowed(false) == ESP_OK);
    assert(event_count == 1U);
    assert(events[0].pin == AIRDAP_PIN_V_SOURCE_STATUS);
    assert(events[0].level == 0U);

    assert(airdap_target_power_set_allowed(true) == ESP_OK);
    assert(event_count == 2U);
    assert(events[1].pin == AIRDAP_PIN_V_SOURCE_STATUS);
    assert(events[1].level == 1U);
}

static void test_target_power_active_reads_shared_status_net(void)
{
    bool active = false;

    reset_fake_gpio();
    input_level = 1;

    assert(airdap_target_power_get_active(&active) == ESP_OK);
    assert(active);
    assert(airdap_target_power_get_active(NULL) == ESP_ERR_INVALID_ARG);
}

static void test_target_reset_accounts_for_inverting_transistor(void)
{
    reset_fake_gpio();

    assert(airdap_target_reset_set_asserted(true) == ESP_OK);
    assert(event_count == 1U);
    assert(events[0].pin == AIRDAP_PIN_TARGET_NRESET);
    assert(events[0].level == 1U);

    assert(airdap_target_reset_set_asserted(false) == ESP_OK);
    assert(event_count == 2U);
    assert(events[1].pin == AIRDAP_PIN_TARGET_NRESET);
    assert(events[1].level == 0U);
}

int main(void)
{
    test_safe_gpio_state();
    test_first_gpio_failure_is_returned();
    test_gpio_config_failure_is_returned();
    test_target_power_control_uses_open_drain_release_levels();
    test_target_power_active_reads_shared_status_net();
    test_target_reset_accounts_for_inverting_transistor();
    test_boot_key_uses_active_low_level();

    puts("board safe-state tests passed");
    return 0;
}
