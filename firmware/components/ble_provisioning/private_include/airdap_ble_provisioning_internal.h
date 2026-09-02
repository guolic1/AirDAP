#pragma once

#ifdef AIRDAP_BLE_PROVISIONING_TESTING

#include <stdbool.h>
#include <stdint.h>

#include "airdap_provisioning_button.h"
#include "esp_err.h"

esp_err_t airdap_ble_provisioning_test_button_action(
    airdap_provisioning_button_action_t action);
void airdap_ble_provisioning_test_network_event(
    int32_t event_id,
    void *event_data);
void airdap_ble_provisioning_test_timeout(void);
bool airdap_ble_provisioning_test_window_active(void);

#endif
