#pragma once

#include <stdbool.h>

#include "airdap_dap_ownership.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AIRDAP_WIFI_STOPPED = 0,
    AIRDAP_WIFI_DISCONNECTED,
    AIRDAP_WIFI_CONNECTING,
    AIRDAP_WIFI_ONLINE,
} airdap_wifi_state_t;

typedef enum {
    AIRDAP_PROVISIONING_IDLE = 0,
    AIRDAP_PROVISIONING_ACTIVE,
    AIRDAP_PROVISIONING_SUCCEEDED,
    AIRDAP_PROVISIONING_TIMED_OUT,
} airdap_provisioning_state_t;

typedef enum {
    AIRDAP_OTA_IDLE = 0,
    AIRDAP_OTA_RECEIVING,
    AIRDAP_OTA_READY_TO_REBOOT,
} airdap_mode_ota_state_t;

typedef enum {
    AIRDAP_MODE_EVENT_USB_ATTACHED = 0,
    AIRDAP_MODE_EVENT_USB_DETACHED,
    AIRDAP_MODE_EVENT_WIFI_STOPPED,
    AIRDAP_MODE_EVENT_WIFI_DISCONNECTED,
    AIRDAP_MODE_EVENT_WIFI_CONNECTING,
    AIRDAP_MODE_EVENT_WIFI_ONLINE,
    AIRDAP_MODE_EVENT_PROVISIONING_STARTED,
    AIRDAP_MODE_EVENT_PROVISIONING_SUCCEEDED,
    AIRDAP_MODE_EVENT_PROVISIONING_TIMED_OUT,
    AIRDAP_MODE_EVENT_PROVISIONING_RESET,
    AIRDAP_MODE_EVENT_OTA_STARTED,
    AIRDAP_MODE_EVENT_OTA_ABORTED,
    AIRDAP_MODE_EVENT_OTA_FAILED,
    AIRDAP_MODE_EVENT_OTA_COMMITTED,
    AIRDAP_MODE_EVENT_OTA_RESET,
} airdap_mode_event_t;

typedef enum {
    AIRDAP_MODE_STATE_OK = 0,
    AIRDAP_MODE_STATE_INVALID_ARGUMENT,
    AIRDAP_MODE_STATE_INVALID_STATE,
    AIRDAP_MODE_STATE_INVALID_TRANSITION,
} airdap_mode_state_result_t;

typedef enum {
    AIRDAP_MODE_DAP_ALLOWED = 0,
    AIRDAP_MODE_DAP_BUSY,
    AIRDAP_MODE_DAP_OFFLINE,
    AIRDAP_MODE_DAP_UNAUTHENTICATED,
    AIRDAP_MODE_DAP_INVALID_ARGUMENT,
    AIRDAP_MODE_DAP_INVALID_STATE,
} airdap_mode_dap_result_t;

typedef struct {
    bool usb_present;
    airdap_wifi_state_t wifi;
    airdap_provisioning_state_t provisioning;
    airdap_mode_ota_state_t ota;
    airdap_dap_owner_t dap_owner;
} airdap_mode_snapshot_t;

void airdap_mode_state_init(void);

airdap_mode_state_result_t airdap_mode_state_transition(
    airdap_mode_event_t event);

airdap_mode_state_result_t airdap_mode_state_get(
    airdap_mode_snapshot_t *snapshot);

/* NETWORK callers must pass true only after authenticating the session. */
airdap_mode_dap_result_t airdap_mode_state_dap_admission(
    airdap_dap_owner_t requested_owner,
    bool authenticated);

/* Applies admission and ownership changes as one policy transaction. USB may
 * conditionally revoke NETWORK ownership after an attach. */
airdap_mode_dap_result_t airdap_mode_state_dap_acquire(
    airdap_dap_owner_t requested_owner,
    bool authenticated,
    airdap_dap_ownership_claim_t *claim);

/* Starts a physical DAP operation only if its claim and mode policy remain
 * valid across the operation-begin transaction. */
airdap_mode_dap_result_t airdap_mode_state_dap_operation_begin(
    airdap_dap_owner_t requested_owner,
    bool authenticated,
    const airdap_dap_ownership_claim_t *claim,
    airdap_dap_ownership_operation_t *operation);

#ifdef __cplusplus
}
#endif
