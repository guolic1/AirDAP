#pragma once

#include <stdint.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint8_t StackType_t;
typedef uint64_t configRUN_TIME_COUNTER_TYPE;
typedef uint32_t TickType_t;

#define configMAX_TASK_NAME_LEN 16
#define configTASKLIST_INCLUDE_COREID 1
#define tskNO_AFFINITY ((BaseType_t) 0x7FFFFFFF)
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t) (ms))

BaseType_t xPortGetCoreID(void);
