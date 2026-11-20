/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_random.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host.h"

#define MAX_TIMERS 8
#define MAX_TASKS 16

int64_t host_now_us;

int64_t esp_timer_get_time(void)
{
    return host_now_us;
}

void vTaskDelay(TickType_t ticks)
{
    host_now_us += (int64_t)ticks * portTICK_PERIOD_MS * 1000;
}

const char *esp_err_to_name(esp_err_t e)
{
    static char name[16];
    snprintf(name, sizeof name, "err%d", e);
    return name;
}

void esp_fill_random(void *buf, size_t len)
{
    memset(buf, 0x11, len);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return malloc(1);
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return malloc(1);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait)
{
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t s)
{
    free(s);
}

struct queue {
    size_t item;
    size_t cap;
    size_t head;
    size_t count;
    unsigned char *buf;
};

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item)
{
    struct queue *q = calloc(1, sizeof * q);
    q->item = item;
    q->cap = len;
    q->buf = calloc(len, item);
    return q;
}

BaseType_t xQueueSend(QueueHandle_t h, const void *item, TickType_t wait)
{
    struct queue *q = h;
    if (q->count == q->cap) {
        return pdFALSE;
    }
    memcpy(q->buf + ((q->head + q->count) % q->cap) * q->item, item, q->item);
    q->count++;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t h, void *item, TickType_t wait)
{
    struct queue *q = h;
    if (q->count == 0) {
        return pdFALSE;
    }
    memcpy(item, q->buf + q->head * q->item, q->item);
    q->head = (q->head + 1) % q->cap;
    q->count--;
    return pdTRUE;
}

void vQueueDelete(QueueHandle_t h)
{
    struct queue *q = h;
    if (q != NULL) {
        free(q->buf);
        free(q);
    }
}

unsigned char *host_queue_storage(QueueHandle_t h, size_t *bytes)
{
    struct queue *q = h;
    *bytes = q->cap * q->item;
    return q->buf;
}

static struct {
    TaskFunction_t fn;
    void *arg;
    bool alive;
} tasks[MAX_TASKS];
static int task_count;
static jmp_buf task_exit;
static bool in_task;

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                       TaskHandle_t *out)
{
    if (task_count == MAX_TASKS) {
        return pdFAIL;
    }
    tasks[task_count].fn = fn;
    tasks[task_count].arg = arg;
    tasks[task_count].alive = true;
    task_count++;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t t)
{
    if (in_task) {
        longjmp(task_exit, 1);
    }
}

int host_tasks_alive(void)
{
    int alive = 0;
    for (int i = 0; i < task_count; i++) {
        alive += tasks[i].alive;
    }
    return alive;
}

void host_run_tasks(void)
{
    for (int i = 0; i < task_count; i++) {
        if (!tasks[i].alive) {
            continue;
        }
        in_task = true;
        if (!setjmp(task_exit)) {
            tasks[i].fn(tasks[i].arg);
        }
        in_task = false;
        tasks[i].alive = false;
    }
}

static struct host_timer timers[MAX_TIMERS];

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].used) {
            memset(&timers[i], 0, sizeof timers[i]);
            timers[i].used = true;
            timers[i].args = *args;
            *out = &timers[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

static esp_err_t timer_start(esp_timer_handle_t t, bool periodic)
{
    if (t == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (t->armed) {
        return ESP_ERR_INVALID_STATE;
    }
    t->armed = true;
    t->periodic = periodic;
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us)
{
    return timer_start(t, false);
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t us)
{
    return timer_start(t, true);
}

esp_err_t esp_timer_stop(esp_timer_handle_t t)
{
    if (t == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!t->armed) {
        return ESP_ERR_INVALID_STATE;
    }
    t->armed = false;
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t t)
{
    if (t == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (t->armed) {
        return ESP_ERR_INVALID_STATE;
    }
    t->used = false;
    return ESP_OK;
}

struct host_timer *host_timer_named(const char *name)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].used && strcmp(timers[i].args.name, name) == 0) {
            return &timers[i];
        }
    }
    return NULL;
}

bool host_timer_fire(struct host_timer *t)
{
    if (t == NULL || !t->armed) {
        return false;
    }
    if (!t->periodic) {
        t->armed = false;
    }
    t->args.callback(t->args.arg);
    return true;
}
