#pragma once
#include "tusb.h"
#include "esp_err.h"
#define TINYUSB_ESPRESSIF_VID 0x303A
#define TINYUSB_EVENT_ATTACHED 1
#define TINYUSB_EVENT_DETACHED 2
typedef struct { int id; } tinyusb_event_t;
typedef struct {
    struct { bool self_powered; int vbus_monitor_io; } phy;
    struct {
        const tusb_desc_device_t *device;
        const char **string;
        int string_count;
        const uint8_t *full_speed_config;
    } descriptor;
    void (*event_cb)(tinyusb_event_t *, void *);
} tinyusb_config_t;
esp_err_t tinyusb_driver_install(const tinyusb_config_t *);
