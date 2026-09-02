#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    const char *key;
    const char *value;
} mdns_txt_item_t;

esp_err_t mdns_init(void);
void mdns_free(void);
esp_err_t mdns_hostname_set(const char *hostname);
esp_err_t mdns_instance_name_set(const char *instance_name);
esp_err_t mdns_service_add(
    const char *instance_name,
    const char *service_type,
    const char *protocol,
    uint16_t port,
    mdns_txt_item_t txt_items[],
    size_t txt_item_count);
esp_err_t mdns_service_remove(
    const char *service_type,
    const char *protocol);
