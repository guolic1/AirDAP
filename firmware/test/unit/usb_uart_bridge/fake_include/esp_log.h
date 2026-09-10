#pragma once

#define ESP_LOGW(tag, format, ...) \
    do { \
        (void) (tag); \
        (void) (format); \
    } while (0)
