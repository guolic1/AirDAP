#pragma once

#define ESP_LOGI(tag, format, ...) \
    do { \
        (void) (tag); \
        (void) (format); \
    } while (0)
