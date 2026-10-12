/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 * SPDX-FileCopyrightText: 2021-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hal_indicator.c
 * @brief Indicator HAL - WS2812 LED and LEDC buzzer implementation
 */

#include "hal_indicator.h"
#include <stdlib.h>
#include <string.h>
#include "driver/ledc.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "hal_indicator";

#define LED_GPIO_NUM            CONFIG_HAL_INDICATOR_LED_GPIO
#define LED_RMT_RESOLUTION_HZ   (10 * 1000 * 1000)
#define LED_TX_TIMEOUT_MS       10
#define LED_BRIGHTNESS_PCT      50
#define LED_PULSE_STEPS         20                          /* ramp up and down, one cycle */
#define LED_PULSE_PEAK          (LED_PULSE_STEPS / 2)
#define LED_LEVEL_FULL          100                         /* the pattern color at LED_BRIGHTNESS_PCT */

#define BUZZER_GPIO_NUM         CONFIG_HAL_INDICATOR_BUZZER_GPIO
#define BUZZER_LEDC_TIMER       LEDC_TIMER_0
#define BUZZER_LEDC_CHANNEL     LEDC_CHANNEL_0
#define BUZZER_LEDC_DUTY_RES    LEDC_TIMER_10_BIT
#define BUZZER_DUTY_FULL        (1u << BUZZER_LEDC_DUTY_RES)  /* 100%, the top of the documented duty range */
#define BUZZER_FREQ_HZ          2700
#define BUZZER_TASK_STACK       2048
#define BUZZER_TASK_PRIORITY    4
#define BUZZER_QUEUE_LEN        5

#if CONFIG_HAL_INDICATOR_BUZZER_PASSIVE
#define BUZZER_DUTY_ON          (BUZZER_DUTY_FULL / 2)
#define BUZZER_DUTY_OFF         0
#define BUZZER_IDLE_LEVEL       0
#elif CONFIG_HAL_INDICATOR_BUZZER_ACTIVE_LOW
#define BUZZER_DUTY_ON          0
#define BUZZER_DUTY_OFF         BUZZER_DUTY_FULL
#define BUZZER_IDLE_LEVEL       1
#else
#define BUZZER_DUTY_ON          BUZZER_DUTY_FULL
#define BUZZER_DUTY_OFF         0
#define BUZZER_IDLE_LEVEL       0
#endif

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

typedef enum {
    LED_MODE_STATIC,
    LED_MODE_BLINK,     /* on for period_ms, then off for period_ms */
    LED_MODE_PULSE,     /* one full ramp cycle takes 2 * period_ms */
} led_mode_t;

typedef struct {
    rgb_t color;
    led_mode_t mode;
    uint16_t period_ms;
} led_pattern_def_t;

typedef struct {
    uint16_t freq_hz;   /* 0 is a silent gap */
    uint16_t duration_ms;
} beep_t;

typedef struct {
    const beep_t *sequence;
    size_t count;
} buzzer_sound_def_t;

static const beep_t BEEP_SHORT[] = {{BUZZER_FREQ_HZ, 100}};
static const beep_t BEEP_2_SHORT[] = {{BUZZER_FREQ_HZ, 100}, {0, 50}, {BUZZER_FREQ_HZ, 100}};
static const beep_t BEEP_2_LONG[] = {{BUZZER_FREQ_HZ, 400}, {0, 100}, {BUZZER_FREQ_HZ, 400}};
static const beep_t BEEP_VERY_SHORT[] = {{BUZZER_FREQ_HZ, 50}};
static const beep_t BEEP_WARNING[] = {{2000, 200}, {0, 100}, {2000, 200}, {0, 100}, {2000, 200}};

#define SOUND(seq) {seq, sizeof(seq) / sizeof(seq[0])}

static const buzzer_sound_def_t s_buzzer_sounds[BUZZER_SOUND_MAX] = {
    [BUZZER_SOUND_TAG_DETECTED] = SOUND(BEEP_SHORT),
    [BUZZER_SOUND_SUCCESS] = SOUND(BEEP_2_SHORT),
    [BUZZER_SOUND_NETWORK_ERROR] = SOUND(BEEP_2_LONG),
    [BUZZER_SOUND_LOW_BATTERY] = SOUND(BEEP_WARNING),
    [BUZZER_SOUND_BUTTON_PRESS] = SOUND(BEEP_VERY_SHORT),
};

static const rgb_t BLACK = {0, 0, 0};

static const led_pattern_def_t s_led_patterns[LED_PATTERN_MAX] = {
    [LED_PATTERN_IDLE] = {{20, 20, 20}, LED_MODE_PULSE, 1000},
    [LED_PATTERN_SCANNING] = {{0, 0, 255}, LED_MODE_PULSE, 500},
    [LED_PATTERN_SUCCESS] = {{0, 255, 0}, LED_MODE_STATIC, 0},
    [LED_PATTERN_NETWORK_ERROR] = {{255, 0, 0}, LED_MODE_BLINK, 500},
    [LED_PATTERN_LOW_BATTERY] = {{255, 0, 0}, LED_MODE_BLINK, 2000},
    [LED_PATTERN_RECONNECTING] = {{150, 75, 0}, LED_MODE_BLINK, 500},
    [LED_PATTERN_OFF] = {{0, 0, 0}, LED_MODE_STATIC, 0},
};

static struct {
    bool initialized;
    hal_led_pattern_t current_pattern;
    rmt_channel_handle_t led_channel;
    rmt_encoder_handle_t led_encoder;
    esp_timer_handle_t led_timer;
    SemaphoreHandle_t mutex;            /* guards the LED state and the pixel buffer */
    QueueHandle_t buzzer_queue;
    bool led_on;
    uint8_t pulse_phase;
    uint8_t pixel_grb[3];               /* RMT reads it after rmt_transmit returns */
} s_indicator;

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} led_strip_encoder_t;

RMT_ENCODER_FUNC_ATTR
static size_t led_strip_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                               const void *primary_data, size_t data_size, rmt_encode_state_t *ret_state)
{
    led_strip_encoder_t *led_encoder = __containerof(encoder, led_strip_encoder_t, base);
    rmt_encoder_handle_t bytes_encoder = led_encoder->bytes_encoder;
    rmt_encoder_handle_t copy_encoder = led_encoder->copy_encoder;
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    switch (led_encoder->state) {
    case 0:
        encoded_symbols += bytes_encoder->encode(bytes_encoder, channel, primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
        __attribute__((fallthrough));
    case 1:
        encoded_symbols += copy_encoder->encode(copy_encoder, channel, &led_encoder->reset_code,
                                                sizeof(led_encoder->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = RMT_ENCODING_RESET;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
    }
out:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t led_strip_encoder_del(rmt_encoder_t *encoder)
{
    led_strip_encoder_t *led_encoder = __containerof(encoder, led_strip_encoder_t, base);
    rmt_del_encoder(led_encoder->bytes_encoder);
    rmt_del_encoder(led_encoder->copy_encoder);
    free(led_encoder);
    return ESP_OK;
}

RMT_ENCODER_FUNC_ATTR
static esp_err_t led_strip_encoder_reset(rmt_encoder_t *encoder)
{
    led_strip_encoder_t *led_encoder = __containerof(encoder, led_strip_encoder_t, base);
    rmt_encoder_reset(led_encoder->bytes_encoder);
    rmt_encoder_reset(led_encoder->copy_encoder);
    led_encoder->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

static esp_err_t create_led_strip_encoder(rmt_encoder_handle_t *ret_encoder)
{
    esp_err_t ret = ESP_OK;
    led_strip_encoder_t *led_encoder = rmt_alloc_encoder_mem(sizeof(led_strip_encoder_t));
    ESP_RETURN_ON_FALSE(led_encoder, ESP_ERR_NO_MEM, TAG, "no mem for LED encoder");

    led_encoder->base.encode = led_strip_encode;
    led_encoder->base.del = led_strip_encoder_del;
    led_encoder->base.reset = led_strip_encoder_reset;

    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = 3,
            .level1 = 0,
            .duration1 = 9,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = 9,
            .level1 = 0,
            .duration1 = 3,
        },
        .flags.msb_first = 1,
    };
    ESP_GOTO_ON_ERROR(rmt_new_bytes_encoder(&bytes_encoder_config, &led_encoder->bytes_encoder),
                      err, TAG, "bytes encoder create failed");

    rmt_copy_encoder_config_t copy_encoder_config = {};
    ESP_GOTO_ON_ERROR(rmt_new_copy_encoder(&copy_encoder_config, &led_encoder->copy_encoder),
                      err, TAG, "copy encoder create failed");

    led_encoder->reset_code = (rmt_symbol_word_t) {
        .level0 = 0,
        .duration0 = 250,
        .level1 = 0,
        .duration1 = 250,
    };

    *ret_encoder = &led_encoder->base;
    return ESP_OK;
err:
    if (led_encoder->bytes_encoder) {
        rmt_del_encoder(led_encoder->bytes_encoder);
    }
    free(led_encoder);
    return ret;
}

/* Both percentages scale a channel: the level of the pattern and the fixed brightness ceiling. */
static uint8_t led_channel(uint8_t value, unsigned level_pct)
{
    return value * level_pct * LED_BRIGHTNESS_PCT / (100 * 100);
}

/* level_pct is 0 to 100 of the pattern color. Caller holds the mutex, or is the only user during init. */
static esp_err_t led_show(const rgb_t *color, unsigned level_pct, int wait_ms)
{
    static const rmt_transmit_config_t tx_config = {.loop_count = 0};

    /* RMT keeps the pixel pointer until the frame is sent, so wait before rewriting it. */
    ESP_RETURN_ON_ERROR(rmt_tx_wait_all_done(s_indicator.led_channel, wait_ms), TAG, "LED frame wait failed");
    s_indicator.pixel_grb[0] = led_channel(color->g, level_pct);
    s_indicator.pixel_grb[1] = led_channel(color->r, level_pct);
    s_indicator.pixel_grb[2] = led_channel(color->b, level_pct);
    return rmt_transmit(s_indicator.led_channel, s_indicator.led_encoder, s_indicator.pixel_grb,
                        sizeof(s_indicator.pixel_grb), &tx_config);
}

static uint64_t led_tick_us(const led_pattern_def_t *def)
{
    uint64_t us = (uint64_t)def->period_ms * 1000;
    return def->mode == LED_MODE_PULSE ? us / LED_PULSE_PEAK : us;
}

static void led_timer_callback(void *arg)
{
    /* Skipping a tick while a pattern change holds the mutex is harmless: the timer is periodic. */
    if (xSemaphoreTake(s_indicator.mutex, 0) != pdTRUE) {
        return;
    }

    /* The esp_timer task must not block, so a frame still in flight skips this tick. */
    esp_err_t ret = rmt_tx_wait_all_done(s_indicator.led_channel, 0);
    if (ret == ESP_ERR_TIMEOUT) {
        xSemaphoreGive(s_indicator.mutex);
        return;
    }

    const led_pattern_def_t *def = &s_led_patterns[s_indicator.current_pattern];

    switch (def->mode) {
    case LED_MODE_PULSE: {
        s_indicator.pulse_phase = (s_indicator.pulse_phase + 1) % LED_PULSE_STEPS;
        unsigned phase = s_indicator.pulse_phase;
        unsigned intensity = (phase < LED_PULSE_PEAK ? phase : LED_PULSE_STEPS - phase) * 10;
        ret = led_show(&def->color, intensity, 0);
        break;
    }
    case LED_MODE_BLINK: {
        s_indicator.led_on = !s_indicator.led_on;
        ret = led_show(s_indicator.led_on ? &def->color : &BLACK, LED_LEVEL_FULL, 0);
        break;
    }
    case LED_MODE_STATIC:
        break;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LED update failed: %s", esp_err_to_name(ret));
    }

    xSemaphoreGive(s_indicator.mutex);
}

/* Only the buzzer task writes the channel. ledc_set_duty_and_update() would fail here, because it needs the fade service. */
static esp_err_t buzzer_set(bool on)
{
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL,
                                      on ? BUZZER_DUTY_ON : BUZZER_DUTY_OFF),
                        TAG, "buzzer duty set failed");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL);
}

static esp_err_t buzzer_play_beep(const beep_t *beep)
{
#if CONFIG_HAL_INDICATOR_BUZZER_PASSIVE
    if (beep->freq_hz > 0) {
        ESP_RETURN_ON_ERROR(ledc_set_freq(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_TIMER, beep->freq_hz),
                            TAG, "buzzer frequency set failed");
    }
#endif
    return buzzer_set(beep->freq_hz > 0);
}

static void buzzer_task_fn(void *arg)
{
    hal_buzzer_sound_t sound;

    while (xQueueReceive(s_indicator.buzzer_queue, &sound, portMAX_DELAY) == pdTRUE) {
        const buzzer_sound_def_t *def = &s_buzzer_sounds[sound];

        for (size_t i = 0; i < def->count; i++) {
            const beep_t *beep = &def->sequence[i];
            esp_err_t err = buzzer_play_beep(beep);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Buzzer update failed: %s", esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(beep->duration_ms));
        }
        esp_err_t err = buzzer_set(false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Buzzer silence failed: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t hal_indicator_init(void)
{
    if (s_indicator.initialized) {
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;

    s_indicator.mutex = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(s_indicator.mutex, ESP_ERR_NO_MEM, err, TAG, "mutex alloc failed");

    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = LED_GPIO_NUM,
        .mem_block_symbols = 64,
        .resolution_hz = LED_RMT_RESOLUTION_HZ,
        .trans_queue_depth = 1,
    };
    ESP_GOTO_ON_ERROR(rmt_new_tx_channel(&tx_chan_config, &s_indicator.led_channel), err, TAG, "RMT channel create failed");
    ESP_GOTO_ON_ERROR(create_led_strip_encoder(&s_indicator.led_encoder), err, TAG, "LED encoder create failed");
    ESP_GOTO_ON_ERROR(rmt_enable(s_indicator.led_channel), err, TAG, "RMT enable failed");

    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BUZZER_LEDC_DUTY_RES,
        .timer_num = BUZZER_LEDC_TIMER,
        .freq_hz = BUZZER_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_GOTO_ON_ERROR(ledc_timer_config(&timer_conf), err, TAG, "LEDC timer config failed");

    ledc_channel_config_t ledc_conf = {
        .gpio_num = BUZZER_GPIO_NUM,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BUZZER_LEDC_CHANNEL,
        .timer_sel = BUZZER_LEDC_TIMER,
        .duty = BUZZER_DUTY_OFF,
        .hpoint = 0,
    };
    ESP_GOTO_ON_ERROR(ledc_channel_config(&ledc_conf), err, TAG, "LEDC channel config failed");

    esp_timer_create_args_t led_timer_args = {
        .callback = led_timer_callback,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "led_pattern",
    };
    ESP_GOTO_ON_ERROR(esp_timer_create(&led_timer_args, &s_indicator.led_timer), err, TAG, "LED timer create failed");

    s_indicator.buzzer_queue = xQueueCreate(BUZZER_QUEUE_LEN, sizeof(hal_buzzer_sound_t));
    ESP_GOTO_ON_FALSE(s_indicator.buzzer_queue, ESP_ERR_NO_MEM, err, TAG, "buzzer queue alloc failed");

    s_indicator.current_pattern = LED_PATTERN_OFF;
    ESP_ERROR_CHECK_WITHOUT_ABORT(led_show(&BLACK, 0, LED_TX_TIMEOUT_MS));

    /* The task starts last, so a failed init never has a running task to stop. */
    ESP_GOTO_ON_FALSE(xTaskCreate(buzzer_task_fn, "buzzer", BUZZER_TASK_STACK, NULL, BUZZER_TASK_PRIORITY,
                                  NULL) == pdPASS,
                      ESP_ERR_NO_MEM, err, TAG, "buzzer task create failed");
    s_indicator.initialized = true;

    ESP_LOGI(TAG, "Initialized (LED=GPIO%d, Buzzer=GPIO%d)", LED_GPIO_NUM, BUZZER_GPIO_NUM);
    return ESP_OK;

err:
    /* Each step releases only what init acquired. No timer callback or task has run yet. */
    if (s_indicator.led_timer) {
        esp_timer_delete(s_indicator.led_timer);
    }
    if (s_indicator.buzzer_queue) {
        vQueueDelete(s_indicator.buzzer_queue);
    }
    ledc_stop(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL, BUZZER_IDLE_LEVEL);
    if (s_indicator.led_channel) {
        rmt_disable(s_indicator.led_channel);
        if (s_indicator.led_encoder) {
            rmt_del_encoder(s_indicator.led_encoder);
        }
        rmt_del_channel(s_indicator.led_channel);
    }
    if (s_indicator.mutex) {
        vSemaphoreDelete(s_indicator.mutex);
    }
    memset(&s_indicator, 0, sizeof(s_indicator));
    return ret;
}

esp_err_t hal_led_set_pattern(hal_led_pattern_t pattern)
{
    if (!s_indicator.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(pattern < LED_PATTERN_MAX, ESP_ERR_INVALID_ARG, TAG, "unknown pattern %d", pattern);

    const led_pattern_def_t *def = &s_led_patterns[pattern];

    /* The timer callback skips its tick while this function holds the mutex. A failed first frame
     * leaves the previous pattern and its timer as they were. */
    xSemaphoreTake(s_indicator.mutex, portMAX_DELAY);

    esp_err_t ret = led_show(&def->color, LED_LEVEL_FULL, LED_TX_TIMEOUT_MS);
    if (ret == ESP_OK) {
        esp_timer_stop(s_indicator.led_timer);
        s_indicator.current_pattern = pattern;
        s_indicator.led_on = true;
        s_indicator.pulse_phase = LED_PULSE_PEAK;
        if (def->mode != LED_MODE_STATIC) {
            ret = esp_timer_start_periodic(s_indicator.led_timer, led_tick_us(def));
        }
    }

    xSemaphoreGive(s_indicator.mutex);

    ESP_LOGD(TAG, "LED pattern: %d", pattern);
    return ret;
}

esp_err_t hal_buzzer_play(hal_buzzer_sound_t sound)
{
    if (!s_indicator.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_FALSE(sound < BUZZER_SOUND_MAX, ESP_ERR_INVALID_ARG, TAG, "unknown sound %d", sound);

    return xQueueSend(s_indicator.buzzer_queue, &sound, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}
