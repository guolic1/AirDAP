#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

typedef struct {
    int unused;
} esp_netif_t;

typedef struct {
    uint32_t addr;
} esp_ip4_addr_t;

#define esp_ip4_addr_get_byte(ipaddr, index) \
    (((const uint8_t *) &(ipaddr)->addr)[index])

typedef struct {
    esp_ip4_addr_t ip;
    esp_ip4_addr_t netmask;
    esp_ip4_addr_t gw;
} esp_netif_ip_info_t;

enum {
    IP_EVENT_STA_GOT_IP = 0,
    IP_EVENT_STA_LOST_IP,
};

extern esp_event_base_t IP_EVENT;

esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
void esp_netif_destroy_default_wifi(void *netif);
esp_err_t esp_netif_get_ip_info(
    esp_netif_t *netif,
    esp_netif_ip_info_t *ip_info);
