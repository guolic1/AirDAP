#pragma once

typedef enum {
    AIRDAP_WIFI_STOPPED = 0,
    AIRDAP_WIFI_DISCONNECTED,
    AIRDAP_WIFI_CONNECTING,
    AIRDAP_WIFI_ONLINE,
} airdap_wifi_state_t;

typedef enum {
    AIRDAP_MODE_STATE_OK = 0,
    AIRDAP_MODE_STATE_INVALID_ARGUMENT,
    AIRDAP_MODE_STATE_INVALID_STATE,
    AIRDAP_MODE_STATE_INVALID_TRANSITION,
} airdap_mode_state_result_t;

typedef struct {
    airdap_wifi_state_t wifi;
} airdap_mode_snapshot_t;

airdap_mode_state_result_t airdap_mode_state_get(
    airdap_mode_snapshot_t *snapshot);
