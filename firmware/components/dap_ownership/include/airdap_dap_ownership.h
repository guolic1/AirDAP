#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AIRDAP_DAP_OWNER_NONE = 0,
    AIRDAP_DAP_OWNER_USB,
    AIRDAP_DAP_OWNER_NETWORK,
    AIRDAP_DAP_OWNER_DIAGNOSTIC,
} airdap_dap_owner_t;

typedef enum {
    AIRDAP_DAP_OWNERSHIP_OK = 0,
    AIRDAP_DAP_OWNERSHIP_ALREADY_OWNER,
    AIRDAP_DAP_OWNERSHIP_BUSY,
    AIRDAP_DAP_OWNERSHIP_NOT_OWNER,
    AIRDAP_DAP_OWNERSHIP_OFFLINE,
    AIRDAP_DAP_OWNERSHIP_INVALID_ARGUMENT,
    AIRDAP_DAP_OWNERSHIP_INVALID_STATE,
} airdap_dap_ownership_result_t;

typedef struct {
    void *context;
    bool (*line_reset)(void *context);
    bool (*release_pins)(void *context);
} airdap_dap_ownership_backend_t;

typedef struct {
    airdap_dap_owner_t owner;
    uint32_t generation;
} airdap_dap_ownership_claim_t;

typedef struct {
    airdap_dap_owner_t owner;
    uint32_t generation;
    bool active;
} airdap_dap_ownership_operation_t;

airdap_dap_ownership_result_t airdap_dap_ownership_initialize(
    const airdap_dap_ownership_backend_t *backend);

airdap_dap_owner_t airdap_dap_ownership_current(void);

airdap_dap_ownership_result_t airdap_dap_ownership_acquire(
    airdap_dap_owner_t owner,
    airdap_dap_ownership_claim_t *claim);

airdap_dap_ownership_result_t airdap_dap_ownership_release(
    const airdap_dap_ownership_claim_t *claim);

/* Holds ownership stable until the matching operation is ended. */
airdap_dap_ownership_result_t airdap_dap_ownership_operation_begin(
    const airdap_dap_ownership_claim_t *claim,
    airdap_dap_ownership_operation_t *operation);

void airdap_dap_ownership_operation_end(
    airdap_dap_ownership_operation_t *operation);

/* Releases the current owner for device-wide transitions such as OTA writes. */
airdap_dap_ownership_result_t airdap_dap_ownership_revoke(void);

/* Releases only the named owner. A concurrent owner change is never revoked. */
airdap_dap_ownership_result_t airdap_dap_ownership_revoke_owner(
    airdap_dap_owner_t owner);

/* Atomically blocks new owners and releases the current owner. A suspended
 * arbiter remains blocked until resume and is used across an OTA write. */
airdap_dap_ownership_result_t airdap_dap_ownership_suspend(void);
airdap_dap_ownership_result_t airdap_dap_ownership_resume(void);

#ifdef __cplusplus
}
#endif
