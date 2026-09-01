#pragma once

typedef enum {
    AIRDAP_MODE_EVENT_WIFI_STOPPED = 0,
    AIRDAP_MODE_EVENT_WIFI_DISCONNECTED,
    AIRDAP_MODE_EVENT_WIFI_CONNECTING,
    AIRDAP_MODE_EVENT_WIFI_ONLINE,
} airdap_mode_event_t;

typedef enum {
    AIRDAP_MODE_STATE_OK = 0,
} airdap_mode_state_result_t;

airdap_mode_state_result_t airdap_mode_state_transition(
    airdap_mode_event_t event);
