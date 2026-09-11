#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;

#define pdFAIL 0
#define pdPASS 1
#define pdMS_TO_TICKS(milliseconds) ((TickType_t) ((milliseconds) / 10U))
