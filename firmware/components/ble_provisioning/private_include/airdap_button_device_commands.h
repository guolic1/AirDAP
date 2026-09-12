#pragma once
#include <stdbool.h>
#include "airdap_button_config.h"

/* Execute on the default event loop. Timed commands return after starting;
 * completion releases their ownership reservation and logs any GPIO failure. */
esp_err_t airdap_button_device_command_execute(airdap_button_command_t command);
bool airdap_button_device_command_busy(void);
