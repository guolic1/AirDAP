#pragma once

#include <stdbool.h>

typedef enum {
    AIRDAP_DAP_OWNER_NONE = 0,
    AIRDAP_DAP_OWNER_USB,
    AIRDAP_DAP_OWNER_NETWORK,
    AIRDAP_DAP_OWNER_DIAGNOSTIC,
} airdap_dap_owner_t;

typedef enum {
    AIRDAP_MODE_DAP_ALLOWED = 0,
    AIRDAP_MODE_DAP_BUSY,
    AIRDAP_MODE_DAP_OFFLINE,
    AIRDAP_MODE_DAP_UNAUTHENTICATED,
    AIRDAP_MODE_DAP_INVALID_ARGUMENT,
    AIRDAP_MODE_DAP_INVALID_STATE,
} airdap_mode_dap_result_t;

airdap_mode_dap_result_t airdap_mode_state_dap_admission(
    airdap_dap_owner_t requested_owner,
    bool authenticated);
