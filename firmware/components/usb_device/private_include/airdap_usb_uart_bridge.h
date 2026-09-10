#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t airdap_usb_uart_bridge_start(void);
void airdap_usb_uart_bridge_disconnected(void);

/* Performs one non-blocking service-to-CDC transfer for host tests and the
 * bridge worker. */
bool airdap_usb_uart_bridge_process_once(void);
