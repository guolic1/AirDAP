#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    AIRDAP_WIFI_RETRY_INITIAL_DELAY_MS = 1000,
    AIRDAP_WIFI_RETRY_MAX_DELAY_MS = 60000,
};

typedef enum {
    AIRDAP_WIFI_SM_STOPPED = 0,
    AIRDAP_WIFI_SM_DISCONNECTED,
    AIRDAP_WIFI_SM_CONNECTING,
    AIRDAP_WIFI_SM_ONLINE,
} airdap_wifi_sm_state_t;

typedef enum {
    AIRDAP_WIFI_FAILURE_NONE = 0,
    AIRDAP_WIFI_FAILURE_AUTHENTICATION,
    AIRDAP_WIFI_FAILURE_TRANSIENT,
} airdap_wifi_failure_t;

typedef enum {
    AIRDAP_WIFI_SM_EVENT_STA_STARTED = 0,
    AIRDAP_WIFI_SM_EVENT_LINK_CONNECTED,
    AIRDAP_WIFI_SM_EVENT_LINK_DISCONNECTED,
    AIRDAP_WIFI_SM_EVENT_GOT_IP,
    AIRDAP_WIFI_SM_EVENT_LOST_IP,
    AIRDAP_WIFI_SM_EVENT_AUTHENTICATION_FAILED,
    AIRDAP_WIFI_SM_EVENT_TRANSIENT_DISCONNECT,
    AIRDAP_WIFI_SM_EVENT_CONNECT_FAILED,
    AIRDAP_WIFI_SM_EVENT_RETRY_EXPIRED,
    AIRDAP_WIFI_SM_EVENT_CONFIGURATION_UPDATED,
    AIRDAP_WIFI_SM_EVENT_CONFIGURATION_CLEARED,
} airdap_wifi_sm_event_t;

typedef struct {
    airdap_wifi_sm_state_t state;
    airdap_wifi_failure_t last_failure;
    uint32_t retry_delay_ms;
    bool has_configuration;
    bool link_connected;
} airdap_wifi_state_machine_t;

typedef struct {
    bool connect;
    bool disconnect;
    bool reconfigure;
    bool cancel_retry;
    bool publish_state;
    airdap_wifi_sm_state_t published_state;
    uint32_t retry_after_ms;
} airdap_wifi_sm_effects_t;

void airdap_wifi_state_machine_init(
    airdap_wifi_state_machine_t *machine,
    bool has_configuration);

bool airdap_wifi_state_machine_step(
    airdap_wifi_state_machine_t *machine,
    airdap_wifi_sm_event_t event,
    airdap_wifi_sm_effects_t *effects);
