/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "hal_button.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "hal_button";

#define BUTTON_GPIO             ((gpio_num_t)CONFIG_HAL_BUTTON_GPIO)
#define DEBOUNCE_TIME_US        (CONFIG_HAL_BUTTON_DEBOUNCE_MS * 1000)
#define HOLD_CHECK_INTERVAL_US  (100 * 1000)
#define HOLD_STEP_MS            1000
#define HOLD_MAX_STEPS          10
#define PRESS_SLOTS             2
#define RETRY_MS                25
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
    int64_t start_us;
    uint32_t release_ms;
    uint8_t steps_seen;
    uint8_t steps_sent;
    bool press_sent;
    bool released;
} press_t;

static struct {
    bool initialized;
    hal_button_event_cb_t event_cb;
    bool pressed;
    bool ignoring_press;
    esp_timer_handle_t debounce_timer;
    esp_timer_handle_t hold_timer;
    TaskHandle_t task;
    portMUX_TYPE lock;
    press_t presses[PRESS_SLOTS];
    uint8_t oldest;
    uint8_t count;
} s_button = {.lock = portMUX_INITIALIZER_UNLOCKED};

static bool read_pressed(void)
{
    int level = gpio_get_level(BUTTON_GPIO);
    return level == BUTTON_PRESSED_LEVEL;
}

static uint8_t steps_in(uint32_t held_ms)
{
    uint32_t steps = held_ms / HOLD_STEP_MS;
    return (uint8_t)(steps < HOLD_MAX_STEPS ? steps : HOLD_MAX_STEPS);
}

static void wake_event_task(void)
{
    xTaskNotifyGive(s_button.task);
}

static press_t *newest_press(void)
{
    return &s_button.presses[(s_button.oldest + s_button.count - 1) % PRESS_SLOTS];
}

static uint32_t held_ms(const press_t *press)
{
    return (uint32_t)((esp_timer_get_time() - press->start_us) / 1000);
}

static bool begin_press(void)
{
    portENTER_CRITICAL(&s_button.lock);
    bool room = s_button.count < PRESS_SLOTS;
    if (room) {
        s_button.count++;
        *newest_press() = (press_t) {
            .start_us = esp_timer_get_time()
        };
    }
    portEXIT_CRITICAL(&s_button.lock);
    return room;
}

static uint8_t record_hold(bool released)
{
    portENTER_CRITICAL(&s_button.lock);
    if (s_button.count == 0 || newest_press()->released) {
        portEXIT_CRITICAL(&s_button.lock);
        return 0;
    }
    press_t *press = newest_press();
    uint32_t ms = held_ms(press);
    uint8_t steps = steps_in(ms);
    press->steps_seen = steps;
    if (released) {
        press->release_ms = ms;
        press->released = true;
    }
    portEXIT_CRITICAL(&s_button.lock);
    return steps;
}

static bool next_event(hal_button_event_t *event, uint32_t *hold_ms)
{
    bool found = true;
    portENTER_CRITICAL(&s_button.lock);
    const press_t *press = &s_button.presses[s_button.oldest];
    if (s_button.count == 0) {
        found = false;
    } else if (!press->press_sent) {
        *event = HAL_BUTTON_EVENT_PRESS;
        *hold_ms = 0;
    } else if (press->steps_sent < press->steps_seen) {
        *event = HAL_BUTTON_EVENT_HOLD;
        *hold_ms = (press->steps_sent + 1) * HOLD_STEP_MS;
    } else if (press->released) {
        *event = HAL_BUTTON_EVENT_RELEASE;
        *hold_ms = press->release_ms;
    } else {
        found = false;
    }
    portEXIT_CRITICAL(&s_button.lock);
    return found;
}

static void mark_accepted(hal_button_event_t event)
{
    portENTER_CRITICAL(&s_button.lock);
    press_t *press = &s_button.presses[s_button.oldest];
    if (event == HAL_BUTTON_EVENT_PRESS) {
        press->press_sent = true;
    } else if (event == HAL_BUTTON_EVENT_HOLD) {
        press->steps_sent++;
    } else {
        s_button.oldest = (s_button.oldest + 1) % PRESS_SLOTS;
        s_button.count--;
    }
    portEXIT_CRITICAL(&s_button.lock);
}

static bool deliver_pending(void)
{
    hal_button_event_t event;
    uint32_t hold_ms;
    while (next_event(&event, &hold_ms)) {
        if (s_button.event_cb(event, hold_ms) != ESP_OK) {
            return false;
        }
        mark_accepted(event);
    }
    return true;
}

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    gpio_intr_disable(BUTTON_GPIO);
    esp_timer_stop(s_button.debounce_timer);
    esp_timer_start_once(s_button.debounce_timer, DEBOUNCE_TIME_US);
}

static void reenable_and_recheck_level(void)
{
    gpio_intr_enable(BUTTON_GPIO);
    if (read_pressed() != s_button.pressed) {
        esp_timer_start_once(s_button.debounce_timer, DEBOUNCE_TIME_US);
    }
}

static void debounce_timer_callback(void *arg)
{
    bool pressed = read_pressed();

    if (pressed && !s_button.pressed) {
        s_button.pressed = true;
        s_button.ignoring_press = !begin_press();
        if (s_button.ignoring_press) {
            ESP_LOGW(TAG, "Two earlier presses still wait for the application, ignoring this press");
        } else {
            esp_timer_start_periodic(s_button.hold_timer, HOLD_CHECK_INTERVAL_US);
            wake_event_task();
        }
    } else if (!pressed && s_button.pressed) {
        s_button.pressed = false;
        if (!s_button.ignoring_press) {
            esp_timer_stop(s_button.hold_timer);
            record_hold(true);
            wake_event_task();
        }
    }

    reenable_and_recheck_level();
}

static void hold_timer_callback(void *arg)
{
    if (record_hold(false) == HOLD_MAX_STEPS) {
        esp_timer_stop(s_button.hold_timer);
    }
    wake_event_task();
}

static void button_event_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (!deliver_pending()) {
            vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
        }
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

    ESP_GOTO_ON_FALSE(xTaskCreate(button_event_task, "btn_event", EVENT_TASK_STACK, NULL,
                                  EVENT_TASK_PRIORITY, &s_button.task) == pdPASS,
                      ESP_ERR_NO_MEM, fail, TAG, "Failed to create event task");

    ret = gpio_isr_handler_add(BUTTON_GPIO, gpio_isr_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO ISR handler add failed");
        vTaskDelete(s_button.task);
        goto fail;
    }

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
    memset(&s_button, 0, sizeof(s_button));
    s_button.lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    return ret;
}
