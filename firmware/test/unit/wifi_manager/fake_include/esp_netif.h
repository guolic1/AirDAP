#pragma once

#include "esp_err.h"
#include "esp_event.h"

typedef struct {
    int unused;
} esp_netif_t;

enum {
    IP_EVENT_STA_GOT_IP = 0,
    IP_EVENT_STA_LOST_IP,
};

extern esp_event_base_t IP_EVENT;

esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
void esp_netif_destroy_default_wifi(void *netif);
