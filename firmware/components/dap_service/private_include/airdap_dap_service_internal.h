#pragma once

#include <stdbool.h>

/* Processes deferred disconnects and at most one queued request. */
bool airdap_dap_service_process_next(void);
