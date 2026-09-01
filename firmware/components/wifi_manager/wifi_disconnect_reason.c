#include <stdbool.h>
#include <stdint.h>

#include "airdap_wifi_disconnect_reason.h"
#include "esp_wifi.h"

bool airdap_wifi_disconnect_is_authentication_failure(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_802_1X_AUTH_FAILED:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return true;
    default:
        return false;
    }
}
