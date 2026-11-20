/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "sdkconfig.h"
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY 0xffffffffu
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef struct {
    int unused;
} portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
extern int host_crit_depth;
#define portENTER_CRITICAL(m) ((void)(m), host_crit_depth++)
#define portEXIT_CRITICAL(m) ((void)(m), host_crit_depth--)
#define IRAM_ATTR
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
typedef struct {
    long opaque[4];
} StaticSemaphore_t;
typedef void (*TaskFunction_t)(void *);
