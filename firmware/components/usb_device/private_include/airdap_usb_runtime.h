#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool airdap_usb_data_ready(void);
/* Runs on the TinyUSB task; also exercised by the host lifecycle test. */
void airdap_usb_reconcile_mode(void *argument);
#if CONFIG_AIRDAP_DEBUG_SHELL
bool airdap_usb_debug_mounted(void);
uint32_t airdap_usb_debug_read(void *buffer, uint32_t size);
uint32_t airdap_usb_debug_write(const void *buffer, uint32_t size);
uint32_t airdap_usb_debug_flush(void);
#endif
