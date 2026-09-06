#pragma once

#include <stdint.h>

typedef enum {
    CHIP_ESP32 = 1,
    CHIP_ESP32S2 = 2,
    CHIP_ESP32S3 = 9,
    CHIP_ESP32C3 = 5,
    CHIP_ESP32C2 = 12,
    CHIP_ESP32C6 = 13,
    CHIP_ESP32H2 = 16,
    CHIP_ESP32P4 = 18,
    CHIP_ESP32C61 = 20,
    CHIP_ESP32C5 = 23,
    CHIP_ESP32H21 = 25,
    CHIP_ESP32H4 = 28,
    CHIP_ESP32S31 = 32,
    CHIP_POSIX_LINUX = 999,
} esp_chip_model_t;

typedef struct {
    esp_chip_model_t model;
    uint32_t features;
    uint16_t revision;
    uint8_t cores;
} esp_chip_info_t;

void esp_chip_info(esp_chip_info_t *info);
