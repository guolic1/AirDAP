#pragma once

#define ESP_LOGI(tag, format, ...) \
    do { \
        (void) (tag); \
        (void) (format); \
    } while (0)

#define ESP_LOGW(tag, format, ...) ESP_LOGI(tag, format, ##__VA_ARGS__)
