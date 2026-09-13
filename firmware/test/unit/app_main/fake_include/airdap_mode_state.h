#pragma once

void airdap_mode_state_init(void);

typedef enum { AIRDAP_MODE_STATE_OK = 0 } airdap_mode_state_result_t;
airdap_mode_state_result_t airdap_mode_state_restore_dap_route(void);
