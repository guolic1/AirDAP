#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "airdap_button_simulation.h"

typedef struct { uint32_t elapsed_ms; bool started; } airdap_button_simulator_t;
/* The button polling task is the sole caller of init/step. */
void airdap_button_simulation_init(void);
/* Returns true when the recognizer must discard the cancelled simulated hold. */
bool airdap_button_simulation_step(airdap_button_simulator_t *simulator,
    bool physical_pressed, bool input_valid, bool recognizer_idle,
    uint32_t elapsed_ms, bool *simulated_pressed);
