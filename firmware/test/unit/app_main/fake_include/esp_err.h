#pragma once

#include <assert.h>

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_ERROR_CHECK(expression) assert((expression) == ESP_OK)
