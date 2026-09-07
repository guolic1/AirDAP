#pragma once

#include <stdbool.h>

typedef struct {
    bool bus_mounted;
    bool suspended;
    bool dap_vendor_mounted;
    bool target_cdc_connected;
    bool debug_vendor_mounted;
    bool dap_session_active;
} airdap_usb_status_t;

void airdap_usb_get_status(airdap_usb_status_t *status);
