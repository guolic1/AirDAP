#pragma once

typedef enum {
    AIRDAP_MODE_EVENT_PROVISIONING_STARTED = 0,
    AIRDAP_MODE_EVENT_PROVISIONING_SUCCEEDED,
    AIRDAP_MODE_EVENT_PROVISIONING_TIMED_OUT,
    AIRDAP_MODE_EVENT_PROVISIONING_RESET,
} airdap_mode_event_t;

typedef enum {
    AIRDAP_MODE_STATE_OK = 0,
} airdap_mode_state_result_t;

airdap_mode_state_result_t airdap_mode_state_transition(
    airdap_mode_event_t event);
