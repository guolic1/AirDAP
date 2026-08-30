#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct esp_partition_t {
    uint32_t address;
    size_t size;
    uint8_t type;
    uint8_t subtype;
    const char *label;
} esp_partition_t;
