#include <stddef.h>
#include <stdatomic.h>
#include "airdap_button_simulator.h"

enum { OFFLINE, IDLE, PHYSICAL_BUSY, GESTURE_BASE };
static atomic_uint control;

void airdap_button_simulation_init(void) { atomic_store(&control, IDLE); }

esp_err_t airdap_button_simulate(airdap_button_gesture_t gesture)
{
    if (gesture < 0 || gesture >= AIRDAP_BUTTON_GESTURE_COUNT) return ESP_ERR_INVALID_ARG;
    unsigned int expected = IDLE;
    return atomic_compare_exchange_strong(&control, &expected, GESTURE_BASE + (unsigned int) gesture)
        ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t airdap_button_simulation_get(airdap_button_gesture_t *gesture)
{
    if (gesture == NULL) return ESP_ERR_INVALID_ARG;
    const unsigned int current = atomic_load(&control);
    if (current == OFFLINE) return ESP_ERR_INVALID_STATE;
    *gesture = current >= GESTURE_BASE ? (airdap_button_gesture_t) (current - GESTURE_BASE)
        : AIRDAP_BUTTON_GESTURE_COUNT;
    return ESP_OK;
}

bool airdap_button_simulation_step(airdap_button_simulator_t *simulator,
    bool physical_pressed, bool input_valid, bool recognizer_idle,
    uint32_t elapsed_ms, bool *simulated_pressed)
{
    *simulated_pressed = false;
    unsigned int current = atomic_load(&control);
    if (current < GESTURE_BASE) {
        if (current != OFFLINE) {
            const unsigned int next = input_valid && !physical_pressed && recognizer_idle ? IDLE : PHYSICAL_BUSY;
            /* A concurrent shell request must not be overwritten by an idle poll. */
            (void) atomic_compare_exchange_strong(&control, &current, next);
        }
        return false;
    }
    if (!simulator->started && !recognizer_idle) {
        /* A request raced the first physical sample. Drop the request without
         * resetting the physical gesture already owned by the recognizer. */
        atomic_store(&control, PHYSICAL_BUSY);
        return false;
    }
    if (!input_valid || physical_pressed) {
        *simulator = (airdap_button_simulator_t) {0};
        atomic_store(&control, PHYSICAL_BUSY);
        return true;
    }
    simulator->started = true;
    const airdap_button_gesture_t gesture = (airdap_button_gesture_t) (current - GESTURE_BASE);
    const uint32_t time = simulator->elapsed_ms;
    uint32_t release_at;
    if (gesture == AIRDAP_BUTTON_GESTURE_DOUBLE) {
        *simulated_pressed = time < 100U || (time >= 200U && time < 300U);
        release_at = 300U;
    } else {
        release_at = gesture == AIRDAP_BUTTON_GESTURE_SINGLE ? 100U :
            gesture == AIRDAP_BUTTON_GESTURE_HOLD2 ? 2100U :
            gesture == AIRDAP_BUTTON_GESTURE_HOLD6 ? 6100U : 10100U;
        *simulated_pressed = time < release_at;
    }
    /* Leave enough release samples for clicks and restart guards to finish. */
    const uint32_t end = release_at + 400U;
    if (time >= end && recognizer_idle) {
        *simulator = (airdap_button_simulator_t) {0};
        atomic_store(&control, IDLE);
    } else if (time < end) {
        simulator->elapsed_ms += elapsed_ms < end - time ? elapsed_ms : end - time;
    }
    return false;
}
