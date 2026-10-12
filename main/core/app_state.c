/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdatomic.h>
#include <string.h>

#include "esp_check.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "hal_ble.h"
#include "hal_button.h"
#include "hal_diagnostics.h"
#include "hal_indicator.h"
#include "hal_network.h"
#include "hal_nfc.h"
#include "hal_power.h"
#include "hal_time.h"

#include "app_state.h"
#include "app_state_priv.h"

static const char *TAG = "app_state";

#define EVENT_QUEUE_SIZE            32
#define EVENT_DATA_MAX              128
#define STATE_TASK_STACK            8192
#define STATE_TASK_PRIORITY         5
#define STATE_TASK_POLL_MS          5000
#define FACTORY_RESET_FEEDBACK_MS   1500
#define QUEUE_OVERFLOW_THRESHOLD    3

typedef enum {
    APP_STATE_INIT,
    APP_STATE_IDLE,
    APP_STATE_ONBOARDING_BLE,
    APP_STATE_CONNECTING_WIFI,
    APP_STATE_WRITING_TAG,
    APP_STATE_ERROR,
    APP_STATE_LOW_BATTERY,
    APP_STATE_COUNT,
} app_state_t;

typedef struct {
    app_event_t event;
    size_t data_len;
    uint8_t data[EVENT_DATA_MAX];
} event_msg_t;

_Static_assert(EVENT_DATA_MAX >= sizeof(hal_ble_wifi_config_t), "event data must hold a WiFi config");
_Static_assert(EVENT_DATA_MAX >= HAL_BLE_DEVICE_ID_MAX, "event data must hold a device id");
_Static_assert(EVENT_DATA_MAX >= sizeof(hal_nfc_err_t), "event data must hold a tag write result");
_Static_assert(APP_STATE_COUNT <= UINT8_MAX && APP_EVENT_COUNT <= UINT8_MAX,
               "hal_diagnostics stores states and events in one byte");

static const char *const s_state_names[APP_STATE_COUNT] = {
    [APP_STATE_INIT] = "INIT",
    [APP_STATE_IDLE] = "IDLE",
    [APP_STATE_ONBOARDING_BLE] = "ONBOARDING_BLE",
    [APP_STATE_CONNECTING_WIFI] = "CONNECTING_WIFI",
    [APP_STATE_WRITING_TAG] = "WRITING_TAG",
    [APP_STATE_ERROR] = "ERROR",
    [APP_STATE_LOW_BATTERY] = "LOW_BATTERY",
};

static const char *const s_event_names[APP_EVENT_COUNT] = {
    [APP_EVENT_INIT_COMPLETE] = "INIT_COMPLETE",
    [APP_EVENT_BUTTON_SHORT] = "BUTTON_SHORT",
    [APP_EVENT_BUTTON_HOLD_3S] = "BUTTON_HOLD_3S",
    [APP_EVENT_BUTTON_HOLD_10S] = "BUTTON_HOLD_10S",
    [APP_EVENT_WIFI_CONNECTING] = "WIFI_CONNECTING",
    [APP_EVENT_WIFI_CONNECTED] = "WIFI_CONNECTED",
    [APP_EVENT_WIFI_DISCONNECTED] = "WIFI_DISCONNECTED",
    [APP_EVENT_WIFI_CONFIG_RECEIVED] = "WIFI_CONFIG_RECEIVED",
    [APP_EVENT_WIFI_SCAN_REQUESTED] = "WIFI_SCAN_REQUESTED",
    [APP_EVENT_URL_SET] = "URL_SET",
    [APP_EVENT_URL_SET_DONE] = "URL_SET_DONE",
    [APP_EVENT_LOW_BATTERY] = "LOW_BATTERY",
    [APP_EVENT_CRITICAL_BATTERY] = "CRITICAL_BATTERY",
};

/* Only the state task reads or writes these. */
static app_state_t s_state = APP_STATE_INIT;
static bool s_critical_battery_pending;  /* The tag write refused a deep sleep */
static bool s_low_battery_pending;       /* The warning came outside IDLE */

static QueueHandle_t s_event_queue;
static bool s_boot_fault;  /* Written before INIT_COMPLETE is queued, read by the state task after */
static atomic_uint s_consecutive_overflows;
static atomic_bool s_overflow_alarm;

static esp_err_t post_event(app_event_t event, const void *data, size_t data_len, TickType_t wait)
{
    ESP_RETURN_ON_FALSE(s_event_queue != NULL, ESP_ERR_INVALID_STATE, TAG, "Event queue not created");
    ESP_RETURN_ON_FALSE(data_len <= EVENT_DATA_MAX, ESP_ERR_INVALID_SIZE, TAG, "Event data too large");

    event_msg_t msg = {.event = event, .data_len = data_len};
    if (data_len > 0) {
        memcpy(msg.data, data, data_len);
    }

    esp_err_t ret = ESP_OK;
    if (xQueueSend(s_event_queue, &msg, wait) == pdTRUE) {
        atomic_store(&s_consecutive_overflows, 0);
    } else {
        hal_diagnostics_record_queue_overflow();
        unsigned overflows = atomic_fetch_add(&s_consecutive_overflows, 1) + 1;
        /* Callers run on NimBLE, esp_event and button tasks, so a stuck queue logs only at powers of two. */
        if ((overflows & (overflows - 1)) == 0) {
            ESP_LOGE(TAG, "Event queue full, dropping %s (%u in a row)", s_event_names[event], overflows);
        }
        if (overflows >= QUEUE_OVERFLOW_THRESHOLD) {
            atomic_store(&s_overflow_alarm, true);
        }
        ret = ESP_ERR_TIMEOUT;
    }

    /* The data can hold WiFi credentials. */
    explicit_bzero(&msg, sizeof(msg));
    return ret;
}

esp_err_t app_post_event(app_event_t event, const void *data, size_t data_len)
{
    return post_event(event, data, data_len, 0);
}

esp_err_t app_post_event_wait(app_event_t event, const void *data, size_t data_len)
{
    return post_event(event, data, data_len, portMAX_DELAY);
}

static void button_callback(hal_button_event_t event, uint32_t hold_time_ms)
{
    if (event == HAL_BUTTON_EVENT_PRESS) {
        ESP_LOGI(TAG, "Button pressed");
    } else if (event == HAL_BUTTON_EVENT_RELEASE) {
        ESP_LOGI(TAG, "Button released (held %lu ms)", (unsigned long)hold_time_ms);
        if (hold_time_ms < 1000) {
            app_post_event(APP_EVENT_BUTTON_SHORT, NULL, 0);
        }
    } else if (hold_time_ms == 3000) {
        ESP_LOGW(TAG, "Button hold: 3 s, BLE onboarding");
        app_post_event(APP_EVENT_BUTTON_HOLD_3S, NULL, 0);
    } else if (hold_time_ms == 10000) {
        ESP_LOGE(TAG, "Button hold: 10 s, factory reset");
        app_post_event(APP_EVENT_BUTTON_HOLD_10S, NULL, 0);
    } else {
        ESP_LOGI(TAG, "Button hold: %lu s", (unsigned long)(hold_time_ms / 1000));
    }
}

/* Runs on the system event task: it only queues the change for the state task. */
static void wifi_event_callback(hal_wifi_state_t state)
{
    switch (state) {
    case HAL_WIFI_STATE_CONNECTING:
        app_post_event(APP_EVENT_WIFI_CONNECTING, NULL, 0);
        break;
    case HAL_WIFI_STATE_CONNECTED:
        app_post_event(APP_EVENT_WIFI_CONNECTED, NULL, 0);
        break;
    case HAL_WIFI_STATE_DISCONNECTED:
    case HAL_WIFI_STATE_ERROR:
        app_post_event(APP_EVENT_WIFI_DISCONNECTED, NULL, 0);
        break;
    }
}

/* Runs on the NimBLE host task: it only queues the request for the state task. */
static void ble_event_callback(hal_ble_event_t event, const void *data, size_t data_len)
{
    switch (event) {
    case HAL_BLE_EVENT_WIFI_SCAN_REQ:
        app_post_event(APP_EVENT_WIFI_SCAN_REQUESTED, NULL, 0);
        break;
    case HAL_BLE_EVENT_WIFI_CONFIG:
        app_post_event(APP_EVENT_WIFI_CONFIG_RECEIVED, data, data_len);
        break;
    case HAL_BLE_EVENT_URL_SET:
        app_post_event(APP_EVENT_URL_SET, data, data_len);
        break;
    }
}

static void set_led(hal_led_pattern_t pattern)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(hal_led_set_pattern(pattern));
}

static void play(hal_buzzer_sound_t sound)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(hal_buzzer_play(sound));
}

static void enter_idle(void)
{
    switch (hal_wifi_get_state()) {
    case HAL_WIFI_STATE_CONNECTED:
        set_led(LED_PATTERN_SUCCESS);
        ESP_LOGI(TAG, "Device idle, WiFi connected");
        break;
    case HAL_WIFI_STATE_CONNECTING:
        set_led(LED_PATTERN_RECONNECTING);
        ESP_LOGI(TAG, "Device idle, WiFi connecting");
        break;
    default:
        set_led(LED_PATTERN_IDLE);
        ESP_LOGI(TAG, "Device idle, WiFi disconnected");
        break;
    }
}

static void enter_onboarding(void)
{
    set_led(LED_PATTERN_SCANNING);
    play(BUZZER_SOUND_TAG_DETECTED);
    /* A client left from an earlier WiFi attempt holds the only connection slot. Advertising
     * resumes when its disconnect event arrives. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(hal_ble_disconnect());
    ESP_ERROR_CHECK_WITHOUT_ABORT(hal_ble_start_advertising());
    ESP_LOGI(TAG, "BLE onboarding mode active");
}

static void enter_error(void)
{
    set_led(LED_PATTERN_NETWORK_ERROR);
    play(BUZZER_SOUND_NETWORK_ERROR);
    ESP_LOGE(TAG, "Device in error state");
}

static void enter_low_battery(void)
{
    set_led(LED_PATTERN_LOW_BATTERY);
    play(BUZZER_SOUND_LOW_BATTERY);
    ESP_LOGW(TAG, "Low battery warning");
}

/* A tag write belongs to the onboarding session: the client stays connected and advertising stays on. */
static bool in_onboarding(app_state_t state)
{
    return state == APP_STATE_ONBOARDING_BLE || state == APP_STATE_WRITING_TAG;
}

/* The cause is the event that made the transition. APP_EVENT_COUNT marks a cause that is no event. */
static void transition_to(app_state_t new_state, app_event_t cause)
{
    if (s_state == new_state) {
        return;
    }

    app_state_t old_state = s_state;
    if (in_onboarding(old_state) && !in_onboarding(new_state)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(hal_ble_stop_advertising());
        /* The client of a WiFi connect attempt stays connected so that it gets the result. */
        if (new_state != APP_STATE_CONNECTING_WIFI) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(hal_ble_disconnect());
        }
    }

    s_state = new_state;
    ESP_LOGI(TAG, "State: %s -> %s", s_state_names[old_state], s_state_names[new_state]);
    hal_diagnostics_record_transition((uint8_t)old_state, (uint8_t)new_state, (uint8_t)cause);

    switch (new_state) {
    case APP_STATE_IDLE:
        enter_idle();
        if (s_low_battery_pending) {
            s_low_battery_pending = false;
            transition_to(APP_STATE_LOW_BATTERY, cause);
        }
        break;
    case APP_STATE_ONBOARDING_BLE:
        if (!in_onboarding(old_state)) {
            enter_onboarding();
        }
        break;
    case APP_STATE_ERROR:
        enter_error();
        break;
    case APP_STATE_LOW_BATTERY:
        enter_low_battery();
        break;
    default:
        break;
    }
}

static void factory_reset(void)
{
    ESP_LOGW(TAG, "Factory reset: erasing NVS and restarting");
    set_led(LED_PATTERN_NETWORK_ERROR);
    play(BUZZER_SOUND_NETWORK_ERROR);
    vTaskDelay(pdMS_TO_TICKS(FACTORY_RESET_FEEDBACK_MS));

    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(err));
        transition_to(APP_STATE_ERROR, APP_EVENT_BUTTON_HOLD_10S);
        return;
    }
    esp_restart();
}

/* Critical battery: stop the radios and sleep. Only a reset wakes the device. */
static void power_down(void)
{
    ESP_LOGW(TAG, "Critical battery, entering deep sleep");
    hal_time_stop_sync();
    hal_ble_deinit();
    hal_network_deinit();
    hal_power_deep_sleep();
}

static void send_wifi_result(bool success)
{
    hal_ble_send_wifi_config_result(success, NULL, success ? "WiFi connected successfully" : "WiFi connection failed");
}

static void handle_wifi_config(const void *data, size_t data_len)
{
    if (data_len != sizeof(hal_ble_wifi_config_t)) {
        ESP_LOGW(TAG, "WiFi config ignored: unexpected size %u", (unsigned)data_len);
        return;
    }

    const hal_ble_wifi_config_t *config = data;
    hal_wifi_credentials_t creds = {0};
    _Static_assert(sizeof(creds.ssid) == sizeof(config->ssid) && sizeof(creds.password) == sizeof(config->password),
                   "BLE and network credentials must have the same layout");
    memcpy(creds.ssid, config->ssid, sizeof(creds.ssid));
    memcpy(creds.password, config->password, sizeof(creds.password));

    esp_err_t err = hal_wifi_connect(&creds);
    explicit_bzero(&creds, sizeof(creds));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed to start: %s", esp_err_to_name(err));
        send_wifi_result(false);
        return;
    }
    transition_to(APP_STATE_CONNECTING_WIFI, APP_EVENT_WIFI_CONFIG_RECEIVED);
}

/* The tag task reported. A deep sleep that came in during the write runs now. */
static void finish_tag_write(const void *data)
{
    hal_nfc_err_t nfc_err;
    memcpy(&nfc_err, data, sizeof(nfc_err));
    bool success = app_url_set_finish(nfc_err);

    if (s_state == APP_STATE_WRITING_TAG) {
        transition_to(APP_STATE_ONBOARDING_BLE, APP_EVENT_URL_SET_DONE);
        set_led(success ? LED_PATTERN_SUCCESS : LED_PATTERN_NETWORK_ERROR);
        play(success ? BUZZER_SOUND_SUCCESS : BUZZER_SOUND_NETWORK_ERROR);
    }
    if (s_critical_battery_pending) {
        power_down();
    }
}

static void handle_state_event(app_event_t event, const void *data, size_t data_len)
{
    switch (s_state) {
    case APP_STATE_INIT:
        if (event == APP_EVENT_INIT_COMPLETE) {
            transition_to(s_boot_fault ? APP_STATE_ERROR : APP_STATE_IDLE, event);
        }
        break;

    case APP_STATE_IDLE:
        if (event == APP_EVENT_BUTTON_HOLD_3S) {
            play(BUZZER_SOUND_BUTTON_PRESS);
            transition_to(APP_STATE_ONBOARDING_BLE, event);
        } else if (event == APP_EVENT_WIFI_CONNECTING || event == APP_EVENT_WIFI_CONNECTED ||
                   event == APP_EVENT_WIFI_DISCONNECTED) {
            enter_idle();
        }
        break;

    case APP_STATE_ONBOARDING_BLE:
        switch (event) {
        case APP_EVENT_BUTTON_SHORT:
        case APP_EVENT_BUTTON_HOLD_3S:
            play(BUZZER_SOUND_BUTTON_PRESS);
            transition_to(APP_STATE_IDLE, event);
            break;
        case APP_EVENT_WIFI_CONFIG_RECEIVED:
            handle_wifi_config(data, data_len);
            break;
        case APP_EVENT_WIFI_SCAN_REQUESTED:
            app_wifi_scan_start();
            break;
        case APP_EVENT_URL_SET:
            if (app_url_set_start(data, data_len)) {
                transition_to(APP_STATE_WRITING_TAG, event);
            }
            break;
        default:
            break;
        }
        break;

    case APP_STATE_WRITING_TAG:
        /* The write ends with URL_SET_DONE. Buttons wait until then. */
        switch (event) {
        case APP_EVENT_URL_SET:
            app_url_set_reject("nfc_config_busy");
            break;
        case APP_EVENT_WIFI_CONFIG_RECEIVED:
            ESP_LOGW(TAG, "WiFi config rejected: a tag write is running");
            send_wifi_result(false);
            break;
        case APP_EVENT_WIFI_SCAN_REQUESTED:
            app_wifi_scan_start();
            break;
        default:
            break;
        }
        break;

    case APP_STATE_CONNECTING_WIFI:
        if (event == APP_EVENT_WIFI_CONNECTED) {
            send_wifi_result(true);
            transition_to(APP_STATE_IDLE, event);
        } else if (event == APP_EVENT_WIFI_DISCONNECTED) {
            /* An event queued before this connect attempt must not end it. */
            hal_wifi_state_t wifi_state = hal_wifi_get_state();
            if (wifi_state != HAL_WIFI_STATE_CONNECTING && wifi_state != HAL_WIFI_STATE_CONNECTED) {
                send_wifi_result(false);
                transition_to(APP_STATE_IDLE, event);
            }
        } else if (event == APP_EVENT_BUTTON_SHORT) {
            transition_to(APP_STATE_IDLE, event);
        }
        break;

    case APP_STATE_ERROR:
    case APP_STATE_LOW_BATTERY:
        if (event == APP_EVENT_BUTTON_SHORT) {
            transition_to(APP_STATE_IDLE, event);
        }
        break;

    default:
        break;
    }
}

static void handle_event(const event_msg_t *msg)
{
    ESP_LOGD(TAG, "Event %s in state %s", s_event_names[msg->event], s_state_names[s_state]);

    /* These events act the same in every state. */
    switch (msg->event) {
    case APP_EVENT_BUTTON_HOLD_10S:
        if (s_state == APP_STATE_WRITING_TAG) {
            ESP_LOGW(TAG, "Factory reset refused: a tag write is running");
        } else {
            factory_reset();
        }
        return;
    case APP_EVENT_CRITICAL_BATTERY:
        if (s_state == APP_STATE_WRITING_TAG) {
            ESP_LOGW(TAG, "Deep sleep postponed: a tag write is running");
            s_critical_battery_pending = true;
        } else {
            power_down();
        }
        return;
    case APP_EVENT_LOW_BATTERY:
        if (s_state == APP_STATE_IDLE) {
            transition_to(APP_STATE_LOW_BATTERY, msg->event);
        } else if (s_state != APP_STATE_ERROR && s_state != APP_STATE_LOW_BATTERY) {
            s_low_battery_pending = true;
        }
        return;
    case APP_EVENT_URL_SET_DONE:
        finish_tag_write(msg->data);
        return;
    case APP_EVENT_WIFI_CONNECTED:
        ESP_ERROR_CHECK_WITHOUT_ABORT(hal_time_start_sync());
        break;
    case APP_EVENT_WIFI_CONFIG_RECEIVED:
    case APP_EVENT_URL_SET:
    case APP_EVENT_WIFI_SCAN_REQUESTED:
        /* A client can stay connected after onboarding ends, so answer instead of dropping. */
        if (s_state != APP_STATE_ONBOARDING_BLE && s_state != APP_STATE_WRITING_TAG) {
            ESP_LOGW(TAG, "%s rejected in state %s", s_event_names[msg->event], s_state_names[s_state]);
            if (msg->event == APP_EVENT_WIFI_CONFIG_RECEIVED) {
                send_wifi_result(false);
            } else if (msg->event == APP_EVENT_URL_SET) {
                app_url_set_reject("not_onboarding");
            } else {
                /* An empty result still carries the done terminator the client waits for. */
                hal_ble_send_wifi_scan_results(NULL, 0);
            }
            return;
        }
        break;
    default:
        break;
    }

    handle_state_event(msg->event, msg->data, msg->data_len);
}

static void state_machine_task(void *arg)
{
    (void)arg;
    event_msg_t msg;

    esp_err_t err = esp_task_wdt_add(NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to subscribe to the task watchdog: %s", esp_err_to_name(err));
    }

    while (1) {
        if (xQueueReceive(s_event_queue, &msg, pdMS_TO_TICKS(STATE_TASK_POLL_MS)) == pdTRUE) {
            handle_event(&msg);
            explicit_bzero(&msg, sizeof(msg));

            /* A tag write must end through URL_SET_DONE, so the alarm waits for it. */
            if (s_state != APP_STATE_WRITING_TAG && atomic_exchange(&s_overflow_alarm, false)) {
                ESP_LOGE(TAG, "Event queue overflowed %d times in a row, entering ERROR state",
                         QUEUE_OVERFLOW_THRESHOLD);
                transition_to(APP_STATE_ERROR, APP_EVENT_COUNT);
            }
        }
        esp_task_wdt_reset();
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Without NVS the device cannot boot to the button that factory-resets it. Everything stored
         * here (Wi-Fi credentials, BLE bonds, device id) comes back through onboarding, so erase. */
        ESP_LOGW(TAG, "NVS needs an erase (%s), stored settings are lost", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase failed");
        err = nvs_flash_init();
    }
    return err;
}

/* Starts what the state task needs to run. A failure here leaves nothing that can show the error
 * except the LED, so the caller returns it. */
static esp_err_t init_core(void)
{
    /* The LED comes first so that every later failure can show itself. */
    ESP_RETURN_ON_ERROR(hal_indicator_init(), TAG, "Indicator init failed");
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "NVS init failed");

    /* Brownout is a power event and not a software fault, so it does not count. */
    esp_reset_reason_t reset_reason = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %d", (int)reset_reason);
    if (reset_reason == ESP_RST_TASK_WDT || reset_reason == ESP_RST_INT_WDT ||
            reset_reason == ESP_RST_WDT || reset_reason == ESP_RST_PANIC) {
        ESP_LOGE(TAG, "Abnormal reset detected (reason=%d)", (int)reset_reason);
        s_boot_fault = true;
    }

    ESP_RETURN_ON_ERROR(hal_diagnostics_init(), TAG, "Diagnostics init failed");

    s_event_queue = xQueueCreate(EVENT_QUEUE_SIZE, sizeof(event_msg_t));
    ESP_RETURN_ON_FALSE(s_event_queue != NULL, ESP_ERR_NO_MEM, TAG, "Event queue create failed");
    ESP_RETURN_ON_FALSE(xTaskCreate(state_machine_task, "app_state", STATE_TASK_STACK, NULL, STATE_TASK_PRIORITY,
                                    NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "State task create failed");
    return ESP_OK;
}

static esp_err_t init_network(void)
{
    hal_wifi_register_callback(wifi_event_callback);
    return hal_network_init();
}

static esp_err_t init_button(void)
{
    return hal_button_init(button_callback);
}

static esp_err_t init_ble(void)
{
    return hal_ble_init(ble_event_callback);
}

#if CONFIG_HAL_POWER_BATTERY_MONITOR
static void battery_callback(hal_battery_level_t level)
{
    app_post_event(level == HAL_BATTERY_CRITICAL ? APP_EVENT_CRITICAL_BATTERY : APP_EVENT_LOW_BATTERY, NULL, 0);
}

static esp_err_t init_power(void)
{
    ESP_RETURN_ON_ERROR(hal_power_init(), TAG, "Power init failed");
    hal_power_register_low_battery_cb(battery_callback);
    return ESP_OK;
}
#endif

/* Returns the first failure after it tried every step. The button must start even when another step
 * fails, because it leaves the error state and runs the factory reset. */
static esp_err_t init_peripherals(void)
{
    static const struct {
        const char *name;
        esp_err_t (*init)(void);
    } steps[] = {
        {"NFC", hal_nfc_init},
        {"Network", init_network},
        {"Button", init_button},
        {"BLE", init_ble},
        {"Device id", app_device_id_publish},
#if CONFIG_HAL_POWER_BATTERY_MONITOR
        {"Power", init_power},
#endif
    };

    esp_err_t first_err = ESP_OK;
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        esp_err_t err = steps[i].init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s init failed: %s", steps[i].name, esp_err_to_name(err));
            if (first_err == ESP_OK) {
                first_err = err;
            }
        }
    }
    return first_err;
}

esp_err_t app_state_init(void)
{
    esp_err_t err = init_core();
    if (err != ESP_OK) {
        /* Shows only when the indicator started. */
        set_led(LED_PATTERN_NETWORK_ERROR);
        return err;
    }

    /* The queue and the state task exist, so the callbacks of these HALs can post at once. */
    err = init_peripherals();
    if (err != ESP_OK) {
        s_boot_fault = true;
    }

    esp_err_t post_err = app_post_event(APP_EVENT_INIT_COMPLETE, NULL, 0);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Application state machine initialized");
        return post_err;
    }
    return err;
}
