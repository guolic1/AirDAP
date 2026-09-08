#pragma once

/* Host-test seams for the synchronous connection and non-blocking revoke
 * paths. Production callers use only airdap_network_dap_start(). */
void airdap_network_dap_handle_socket(int socket_fd);
void airdap_network_dap_process_revocations(void);
