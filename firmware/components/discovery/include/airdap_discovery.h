#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    AIRDAP_DISCOVERY_DAP_TCP_PORT = 3260,
    AIRDAP_DISCOVERY_UART_TCP_PORT = 3261,
    AIRDAP_DISCOVERY_HOSTNAME_SIZE = 24,
};

typedef struct {
    bool initialized;
    bool started;
    bool service_published;
    char hostname[AIRDAP_DISCOVERY_HOSTNAME_SIZE];
    uint16_t dap_port;
    uint16_t uart_port;
    esp_err_t last_error;
} airdap_discovery_status_t;

esp_err_t airdap_discovery_start(void);
esp_err_t airdap_discovery_get_status(airdap_discovery_status_t *status);

#ifdef __cplusplus
}
#endif
