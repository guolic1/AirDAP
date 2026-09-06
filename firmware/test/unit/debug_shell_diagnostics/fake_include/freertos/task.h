#pragma once

#include "freertos/FreeRTOS.h"

typedef enum {
    eRunning = 0,
    eReady,
    eBlocked,
    eSuspended,
    eDeleted,
    eInvalid,
} eTaskState;

typedef struct {
    const char *pcTaskName;
    UBaseType_t xTaskNumber;
    eTaskState eCurrentState;
    UBaseType_t uxCurrentPriority;
    UBaseType_t uxBasePriority;
    configRUN_TIME_COUNTER_TYPE ulRunTimeCounter;
    unsigned int usStackHighWaterMark;
    BaseType_t xCoreID;
} TaskStatus_t;

UBaseType_t uxTaskGetNumberOfTasks(void);
UBaseType_t uxTaskGetSystemState(
    TaskStatus_t *tasks,
    UBaseType_t capacity,
    configRUN_TIME_COUNTER_TYPE *total_runtime);
void vTaskSuspendAll(void);
BaseType_t xTaskResumeAll(void);
