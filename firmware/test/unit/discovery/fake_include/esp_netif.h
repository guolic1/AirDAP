#pragma once

#include "esp_event.h"

ESP_EVENT_DECLARE_BASE(IP_EVENT);

enum {
    IP_EVENT_STA_GOT_IP = 1,
    IP_EVENT_STA_LOST_IP = 2,
};
