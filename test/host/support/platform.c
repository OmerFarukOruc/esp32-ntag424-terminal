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
int host_crit_depth;
int host_timer_start_fails;
esp_err_t host_timer_start_error = ESP_FAIL;
int host_timer_delete_fails;
void (*host_timer_fired_hook)(void);
const char *host_timer_stop_hook_name;
void (*host_timer_stop_hook)(void);
TickType_t host_queue_last_wait;
bool host_in_host;
bool host_blocked_in_host;
void (*host_on_block)(void);
void (*host_on_delay)(void);
jmp_buf *host_task_blocks;

int64_t esp_timer_get_time(void)
{
    return host_now_us;
}

void vTaskDelay(TickType_t ticks)
{
    host_blocked_in_host |= host_in_host && ticks > 0;
    if (host_on_delay != NULL) {
        host_on_delay();
    }
    host_now_us += (int64_t)ticks * portTICK_PERIOD_MS * 1000;
}

const char *esp_err_to_name(esp_err_t e)
{
    static char names[16][16];
    static unsigned next;
    char *name = names[next++ % 16];
    snprintf(name, sizeof names[0], "err%d", e);
    return name;
}

void esp_fill_random(void *buf, size_t len)
{
    memset(buf, 0x11, len);
}

struct host_sem {
    bool binary;
    int count;
};

static struct host_sem *sem_init(struct host_sem *s, bool binary)
{
    s->binary = binary;
    s->count = 0;
    return s;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return sem_init(malloc(sizeof(struct host_sem)), false);
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return sem_init(malloc(sizeof(struct host_sem)), true);
}

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *buf)
{
    _Static_assert(sizeof(StaticSemaphore_t) >= sizeof(struct host_sem), "StaticSemaphore_t too small");
    return sem_init((struct host_sem *)buf, true);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t wait)
{
    struct host_sem *s = h;
    if (!s->binary) {
        return pdTRUE;
    }
    if (s->count == 0 && wait > 0) {
        host_blocked_in_host |= host_in_host;
        if (host_on_block != NULL) {
            host_on_block();
        }
    }
    if (s->count > 0) {
        s->count--;
        return pdTRUE;
    }
    vTaskDelay(wait);
    return pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t h)
{
    struct host_sem *s = h;
    if (s->binary) {
        if (s->count == 1) {
            return pdFALSE;
        }
        s->count = 1;
    }
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
    host_queue_last_wait = wait;
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
    if (out != NULL) {
        *out = &tasks[task_count];
    }
    task_count++;
    return pdPASS;
}

int host_task_wakeups;

static uint32_t notifications;

BaseType_t xTaskNotifyGive(TaskHandle_t t)
{
    host_task_wakeups++;
    notifications++;
    return pdPASS;
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    if (wait == portMAX_DELAY && notifications == 0) {
        longjmp(*host_task_blocks, 1);
    }
    if (wait != portMAX_DELAY && notifications == 0) {
        if (host_on_delay != NULL) {
            host_on_delay();
        }
        if (notifications == 0) {
            host_now_us += (int64_t)wait * portTICK_PERIOD_MS * 1000;
            return 0;
        }
    }
    uint32_t taken = notifications;
    notifications = clear ? 0 : notifications - 1;
    return taken;
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

static esp_err_t timer_start(esp_timer_handle_t t, uint64_t us, bool periodic)
{
    if (t == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (t->armed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (host_timer_start_fails > 0) {
        host_timer_start_fails--;
        return host_timer_start_error;
    }
    t->armed = true;
    t->periodic = periodic;
    t->delay_us = us;
    t->due_us = host_now_us + (int64_t)us;
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us)
{
    return timer_start(t, us, false);
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t us)
{
    return timer_start(t, us, true);
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

esp_err_t esp_timer_stop_blocking(esp_timer_handle_t t, uint32_t timeout_ticks)
{
    if (t == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    t->armed = false;
    if (host_timer_stop_hook != NULL && strcmp(t->args.name, host_timer_stop_hook_name) == 0) {
        void (*hook)(void) = host_timer_stop_hook;
        host_timer_stop_hook = NULL;
        hook();
    }
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t t)
{
    if (t == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (t->armed || host_timer_delete_fails > 0) {
        host_timer_delete_fails -= host_timer_delete_fails > 0;
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

void host_advance(int64_t us)
{
    int64_t target = host_now_us + us;
    for (;;) {
        struct host_timer *next = NULL;
        for (int i = 0; i < MAX_TIMERS; i++) {
            if (timers[i].used && timers[i].armed && timers[i].due_us <= target &&
                    (next == NULL || timers[i].due_us < next->due_us)) {
                next = &timers[i];
            }
        }
        if (next == NULL) {
            break;
        }
        if (next->due_us > host_now_us) {
            host_now_us = next->due_us;
        }
        host_timer_fire(next);
        if (host_timer_fired_hook != NULL) {
            host_timer_fired_hook();
        }
    }
    host_now_us = target;
}
