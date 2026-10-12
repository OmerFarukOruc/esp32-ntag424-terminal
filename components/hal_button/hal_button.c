/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hal_button.c
 * @brief Button HAL implementation with debounce and hold detection
 */

#include "hal_button.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "hal_button";

#define BUTTON_GPIO             ((gpio_num_t)CONFIG_HAL_BUTTON_GPIO)
#define DEBOUNCE_TIME_US        (CONFIG_HAL_BUTTON_DEBOUNCE_MS * 1000)
#define HOLD_CHECK_INTERVAL_US  (100 * 1000)
#define HOLD_STEP_MS            1000
#define HOLD_MAX_STEPS          10
#define EVENT_QUEUE_LEN         10
#define EVENT_TASK_STACK        4096
#define EVENT_TASK_PRIORITY     5

_Static_assert(GPIO_IS_VALID_GPIO(CONFIG_HAL_BUTTON_GPIO), "HAL_BUTTON_GPIO is not a valid GPIO");
/* A disabled Kconfig bool is not defined at all, so it can only be tested with #if. */
#if CONFIG_HAL_BUTTON_ACTIVE_LOW
#define BUTTON_PRESSED_LEVEL    0
#else
#define BUTTON_PRESSED_LEVEL    1
#endif
#if CONFIG_HAL_BUTTON_INTERNAL_PULLUP
#define BUTTON_PULL_UP          GPIO_PULLUP_ENABLE
#else
#define BUTTON_PULL_UP          GPIO_PULLUP_DISABLE
#endif

typedef struct {
    hal_button_event_t event;
    uint32_t hold_ms;
} button_msg_t;

static struct {
    bool initialized;
    hal_button_event_cb_t event_cb;
    bool pressed;
    int64_t press_start_us;
    int last_hold_step;
    esp_timer_handle_t debounce_timer;
    esp_timer_handle_t hold_timer;
    QueueHandle_t queue;
} s_button;

static bool read_pressed(void)
{
    int level = gpio_get_level(BUTTON_GPIO);
    return level == BUTTON_PRESSED_LEVEL;
}

static uint32_t hold_duration_ms(void)
{
    return (uint32_t)((esp_timer_get_time() - s_button.press_start_us) / 1000);
}

static void queue_event(hal_button_event_t event, uint32_t hold_ms)
{
    button_msg_t msg = { .event = event, .hold_ms = hold_ms };
    if (xQueueSend(s_button.queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full, dropped event %d", (int)event);
    }
}

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    gpio_intr_disable(BUTTON_GPIO);
    esp_timer_stop(s_button.debounce_timer);
    esp_timer_start_once(s_button.debounce_timer, DEBOUNCE_TIME_US);
}

static void debounce_timer_callback(void *arg)
{
    bool pressed = read_pressed();

    if (pressed && !s_button.pressed) {
        s_button.pressed = true;
        s_button.press_start_us = esp_timer_get_time();
        s_button.last_hold_step = 0;
        queue_event(HAL_BUTTON_EVENT_PRESS, 0);
        esp_timer_start_periodic(s_button.hold_timer, HOLD_CHECK_INTERVAL_US);
    } else if (!pressed && s_button.pressed) {
        uint32_t held_ms = hold_duration_ms();
        s_button.pressed = false;
        esp_timer_stop(s_button.hold_timer);
        queue_event(HAL_BUTTON_EVENT_RELEASE, held_ms);
    }

    gpio_intr_enable(BUTTON_GPIO);

    /* An edge between the read above and the re-enable raised no interrupt. */
    if (read_pressed() != s_button.pressed) {
        esp_timer_start_once(s_button.debounce_timer, DEBOUNCE_TIME_US);
    }
}

static void hold_timer_callback(void *arg)
{
    int step = hold_duration_ms() / HOLD_STEP_MS;
    if (step > HOLD_MAX_STEPS) {
        step = HOLD_MAX_STEPS;
    }

    /* A late tick can cross two steps at once, and then only the latest is reported. */
    if (step > s_button.last_hold_step) {
        s_button.last_hold_step = step;
        queue_event(HAL_BUTTON_EVENT_HOLD, step * HOLD_STEP_MS);
        if (step == HOLD_MAX_STEPS) {
            esp_timer_stop(s_button.hold_timer);
        }
    }
}

static void button_event_task(void *arg)
{
    button_msg_t msg;

    for (;;) {
        if (xQueueReceive(s_button.queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        s_button.event_cb(msg.event, msg.hold_ms);
    }
}

esp_err_t hal_button_init(hal_button_event_cb_t event_cb)
{
    ESP_RETURN_ON_FALSE(event_cb != NULL, ESP_ERR_INVALID_ARG, TAG, "Event callback is required");

    if (s_button.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;

    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << CONFIG_HAL_BUTTON_GPIO,
                             .mode = GPIO_MODE_INPUT,
                             .pull_up_en = BUTTON_PULL_UP,
                             .pull_down_en = GPIO_PULLDOWN_DISABLE,
                             .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "GPIO config failed");

    s_button.event_cb = event_cb;
    s_button.queue = xQueueCreate(EVENT_QUEUE_LEN, sizeof(button_msg_t));
    ESP_GOTO_ON_FALSE(s_button.queue, ESP_ERR_NO_MEM, fail, TAG, "Failed to create event queue");

    const esp_timer_create_args_t debounce_args = {
        .callback = debounce_timer_callback,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "btn_debounce",
    };
    ESP_GOTO_ON_ERROR(esp_timer_create(&debounce_args, &s_button.debounce_timer), fail, TAG,
                      "Debounce timer create failed");

    const esp_timer_create_args_t hold_args = {
        .callback = hold_timer_callback,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "btn_hold",
    };
    ESP_GOTO_ON_ERROR(esp_timer_create(&hold_args, &s_button.hold_timer), fail, TAG,
                      "Hold timer create failed");

    ret = gpio_install_isr_service(0);
    ESP_GOTO_ON_FALSE(ret == ESP_OK || ret == ESP_ERR_INVALID_STATE, ret, fail, TAG,
                      "GPIO ISR service install failed");
    ESP_GOTO_ON_ERROR(gpio_isr_handler_add(BUTTON_GPIO, gpio_isr_handler, NULL), fail, TAG,
                      "GPIO ISR handler add failed");

    /* The task keeps callbacks off the esp_timer task, which must not block. It starts last, so a
     * failed init never has a task to stop. */
    ESP_GOTO_ON_FALSE(xTaskCreate(button_event_task, "btn_event", EVENT_TASK_STACK, NULL,
                                  EVENT_TASK_PRIORITY, NULL) == pdPASS,
                      ESP_ERR_NO_MEM, fail, TAG, "Failed to create event task");

    s_button.initialized = true;
    ESP_LOGI(TAG, "Initialized on GPIO%d (active_low=%d, pullup=%d, debounce=%dms)",
             CONFIG_HAL_BUTTON_GPIO, BUTTON_PRESSED_LEVEL == 0,
             BUTTON_PULL_UP == GPIO_PULLUP_ENABLE, CONFIG_HAL_BUTTON_DEBOUNCE_MS);
    return ESP_OK;

fail:
    gpio_isr_handler_remove(BUTTON_GPIO);
    if (s_button.debounce_timer) {
        esp_timer_stop(s_button.debounce_timer);
        esp_timer_delete(s_button.debounce_timer);
    }
    if (s_button.hold_timer) {
        esp_timer_stop(s_button.hold_timer);
        esp_timer_delete(s_button.hold_timer);
    }
    if (s_button.queue) {
        vQueueDelete(s_button.queue);
    }
    memset(&s_button, 0, sizeof(s_button));
    return ret;
}
