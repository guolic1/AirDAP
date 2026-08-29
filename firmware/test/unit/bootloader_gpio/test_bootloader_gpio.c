#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hal/gpio_ll.h"
#include "soc/io_mux_reg.h"

enum {
    PIN_POWER_STATUS = 9,
    PIN_SWCLK = 12,
    PIN_SWDIO = 13,
    PIN_SWDIO_DIR = 14,
    PIN_UART_TX = 17,
    PIN_UART_RX = 18,
    PIN_TARGET_NRESET = 41,
    PIN_COUNT = 49,
    EVENT_CAPACITY = 128,
};

typedef enum {
    EVENT_FUNCTION,
    EVENT_INPUT_DISABLE,
    EVENT_INPUT_ENABLE,
    EVENT_OD_DISABLE,
    EVENT_OD_ENABLE,
    EVENT_OUTPUT_DISABLE,
    EVENT_OUTPUT_ENABLE,
    EVENT_PULLDOWN_DISABLE,
    EVENT_PULLUP_DISABLE,
    EVENT_LEVEL_LOW,
    EVENT_LEVEL_HIGH,
} event_type_t;

typedef struct {
    event_type_t type;
    uint32_t pin;
} event_t;

typedef struct {
    bool function_is_gpio;
    bool input_enabled;
    bool open_drain;
    bool output_enabled;
    bool pulldown_enabled;
    bool pullup_enabled;
    uint32_t level;
} pin_state_t;

gpio_dev_t fake_gpio_device;

static pin_state_t pin_states[PIN_COUNT];
static event_t events[EVENT_CAPACITY];
static size_t event_count;

void bootloader_before_init(void);
void bootloader_hooks_include(void);

static void record_event(event_type_t type, uint32_t pin)
{
    assert(pin < PIN_COUNT);
    assert(event_count < EVENT_CAPACITY);
    events[event_count++] = (event_t) {.type = type, .pin = pin};
}

void gpio_ll_func_sel(gpio_dev_t *hw, uint8_t gpio_num, uint32_t function)
{
    assert(hw == &fake_gpio_device);
    assert(function == PIN_FUNC_GPIO);
    pin_states[gpio_num].function_is_gpio = true;
    record_event(EVENT_FUNCTION, gpio_num);
}

void gpio_ll_input_disable(gpio_dev_t *hw, uint32_t gpio_num)
{
    assert(hw == &fake_gpio_device);
    pin_states[gpio_num].input_enabled = false;
    record_event(EVENT_INPUT_DISABLE, gpio_num);
}

void gpio_ll_input_enable(gpio_dev_t *hw, uint32_t gpio_num)
{
    assert(hw == &fake_gpio_device);
    pin_states[gpio_num].input_enabled = true;
    record_event(EVENT_INPUT_ENABLE, gpio_num);
}

void gpio_ll_od_disable(gpio_dev_t *hw, uint32_t gpio_num)
{
    assert(hw == &fake_gpio_device);
    pin_states[gpio_num].open_drain = false;
    record_event(EVENT_OD_DISABLE, gpio_num);
}

void gpio_ll_od_enable(gpio_dev_t *hw, uint32_t gpio_num)
{
    assert(hw == &fake_gpio_device);
    pin_states[gpio_num].open_drain = true;
    record_event(EVENT_OD_ENABLE, gpio_num);
}

void gpio_ll_output_disable(gpio_dev_t *hw, uint32_t gpio_num)
{
    assert(hw == &fake_gpio_device);
    pin_states[gpio_num].output_enabled = false;
    record_event(EVENT_OUTPUT_DISABLE, gpio_num);
}

void gpio_ll_output_enable(gpio_dev_t *hw, uint32_t gpio_num)
{
    assert(hw == &fake_gpio_device);
    pin_states[gpio_num].output_enabled = true;
    record_event(EVENT_OUTPUT_ENABLE, gpio_num);
}

void gpio_ll_pulldown_dis(gpio_dev_t *hw, uint32_t gpio_num)
{
    assert(hw == &fake_gpio_device);
    pin_states[gpio_num].pulldown_enabled = false;
    record_event(EVENT_PULLDOWN_DISABLE, gpio_num);
}

void gpio_ll_pullup_dis(gpio_dev_t *hw, uint32_t gpio_num)
{
    assert(hw == &fake_gpio_device);
    pin_states[gpio_num].pullup_enabled = false;
    record_event(EVENT_PULLUP_DISABLE, gpio_num);
}

void gpio_ll_set_level(gpio_dev_t *hw, uint32_t gpio_num, uint32_t level)
{
    assert(hw == &fake_gpio_device);
    assert(level <= 1U);
    pin_states[gpio_num].level = level;
    record_event(level == 0U ? EVENT_LEVEL_LOW : EVENT_LEVEL_HIGH, gpio_num);
}

static size_t find_event(event_type_t type, uint32_t pin)
{
    for (size_t index = 0; index < event_count; ++index) {
        if (events[index].type == type && events[index].pin == pin) {
            return index;
        }
    }

    assert(false);
    return EVENT_CAPACITY;
}

static void assert_push_pull_output(uint32_t pin, uint32_t level)
{
    const pin_state_t *state = &pin_states[pin];

    assert(state->function_is_gpio);
    assert(!state->input_enabled);
    assert(!state->open_drain);
    assert(state->output_enabled);
    assert(!state->pulldown_enabled);
    assert(!state->pullup_enabled);
    assert(state->level == level);

    assert(find_event(EVENT_OUTPUT_DISABLE, pin) < find_event(level == 0U ? EVENT_LEVEL_LOW : EVENT_LEVEL_HIGH, pin));
    assert(find_event(level == 0U ? EVENT_LEVEL_LOW : EVENT_LEVEL_HIGH, pin) < find_event(EVENT_OUTPUT_ENABLE, pin));
}

static void assert_high_impedance_input(uint32_t pin)
{
    const pin_state_t *state = &pin_states[pin];

    assert(state->function_is_gpio);
    assert(state->input_enabled);
    assert(!state->open_drain);
    assert(!state->output_enabled);
    assert(!state->pulldown_enabled);
    assert(!state->pullup_enabled);
}

static void assert_open_drain_released(uint32_t pin)
{
    const pin_state_t *state = &pin_states[pin];

    assert(state->function_is_gpio);
    assert(state->input_enabled);
    assert(state->open_drain);
    assert(state->output_enabled);
    assert(!state->pulldown_enabled);
    assert(!state->pullup_enabled);
    assert(state->level == 1U);

    assert(find_event(EVENT_OUTPUT_DISABLE, pin) < find_event(EVENT_LEVEL_HIGH, pin));
    assert(find_event(EVENT_LEVEL_HIGH, pin) < find_event(EVENT_OD_ENABLE, pin));
    assert(find_event(EVENT_OD_ENABLE, pin) < find_event(EVENT_OUTPUT_ENABLE, pin));
}

int main(void)
{
    for (size_t pin = 0; pin < PIN_COUNT; ++pin) {
        pin_states[pin] = (pin_state_t) {
            .function_is_gpio = false,
            .input_enabled = true,
            .open_drain = true,
            .output_enabled = true,
            .pulldown_enabled = true,
            .pullup_enabled = true,
            .level = 2U,
        };
    }

    bootloader_hooks_include();
    bootloader_before_init();

    assert_push_pull_output(PIN_SWCLK, 0U);
    assert_high_impedance_input(PIN_SWDIO);
    assert_push_pull_output(PIN_SWDIO_DIR, 0U);
    assert_push_pull_output(PIN_TARGET_NRESET, 0U);
    assert_open_drain_released(PIN_POWER_STATUS);
    assert_push_pull_output(PIN_UART_TX, 1U);
    assert_high_impedance_input(PIN_UART_RX);

    puts("bootloader GPIO safe-state tests passed");
    return 0;
}
