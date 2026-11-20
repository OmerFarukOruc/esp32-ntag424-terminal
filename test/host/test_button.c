/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include HAL_BUTTON_SRC
#include "check.h"
#include "host.h"

#define RELEASED 1
#define PRESSED 0

static char seen[256];
static int64_t pressed_at;
static hal_button_event_t refuse_event;
static uint32_t refuse_ms;
static int refusals_left;
static jmp_buf task_blocks;
static int hold_ticks_during_delay;

static esp_err_t on_button(hal_button_event_t event, uint32_t hold_ms)
{
    if (refusals_left > 0 && event == refuse_event && hold_ms == refuse_ms) {
        refusals_left--;
        return ESP_ERR_TIMEOUT;
    }
    size_t n = strlen(seen);
    const char *name = event == HAL_BUTTON_EVENT_PRESS ? "P" : event == HAL_BUTTON_EVENT_HOLD ? "H" : "R";
    snprintf(seen + n, sizeof seen - n, "%s%s%u", n == 0 || seen[n - 1] == ';' ? "" : ",", name, (unsigned)hold_ms);
    return ESP_OK;
}

static void refuse(hal_button_event_t event, uint32_t hold_ms, int times)
{
    refuse_event = event;
    refuse_ms = hold_ms;
    refusals_left = times;
}

static void run_event_task(void)
{
    host_task_blocks = &task_blocks;
    if (!setjmp(task_blocks)) {
        button_event_task(NULL);
    }
}

static void tick_while_task_waits(void)
{
    if (hold_ticks_during_delay > 0) {
        hold_ticks_during_delay--;
        host_timer_fire(host_timer_named("btn_hold"));
    }
}

static bool paused;

static void edge(int level)
{
    host_gpio_level = level;
    host_gpio_isr(NULL);
    host_timer_fire(host_timer_named("btn_debounce"));
    if (!paused) {
        run_event_task();
    }
}

static void press(int64_t at_us)
{
    seen[0] = '\0';
    host_now_us = at_us;
    pressed_at = at_us;
    edge(PRESSED);
}

static void tick_at(int64_t ms_after_press)
{
    host_now_us = pressed_at + ms_after_press * 1000;
    strcat(seen, ";");
    host_timer_fire(host_timer_named("btn_hold"));
    if (!paused) {
        run_event_task();
    }
}

static void release_at(int64_t ms_after_press)
{
    host_now_us = pressed_at + ms_after_press * 1000;
    strcat(seen, ";");
    edge(RELEASED);
}

int main(void)
{
    hal_button_init(on_button);

    host_now_us = 500000;
    hold_timer_callback(NULL);
    run_event_task();
    check("a hold tick with no press waiting delivers nothing", seen[0] == '\0', seen);

    press(1000000);
    tick_at(2000);
    tick_at(4050);
    release_at(4100);
    check("a late tick reports every crossed second in order", strcmp(seen, "P0;H1000,H2000;H3000,H4000;R4100") == 0,
          seen);

    press(9000000);
    tick_at(1000);
    tick_at(2950);
    release_at(3020);
    check("a release before the 3 s tick still reports 3 s first", strcmp(seen, "P0;H1000;H2000;H3000,R3020") == 0, seen);

    press(20000000);
    for (int ms = 100; ms <= 1500; ms += 100) {
        tick_at(ms);
    }
    release_at(1500);
    check("ticks on time give one event per second",
          strcmp(seen, "P0;;;;;;;;;;H1000;;;;;;R1500") == 0, seen);

    paused = true;
    press(30000000);
    for (int ms = 1000; ms <= 10000; ms += 1000) {
        tick_at(ms);
    }
    bool hold_timer_after_10s = host_timer_named("btn_hold")->armed;
    release_at(10500);
    press(50000000);
    tick_at(1000);
    tick_at(3050);
    release_at(3100);
    paused = false;
    run_event_task();
    check("a press made while the last one still waits keeps its 3 s milestone",
          strcmp(seen, ";;;P0,H1000,H2000,H3000,H4000,H5000,H6000,H7000,H8000,H9000,H10000,R10500,"
                 "P0,H1000,H2000,H3000,R3100") == 0 && !hold_timer_after_10s, seen);

    paused = true;
    press(70000000);
    for (int ms = 1000; ms <= 10000; ms += 1000) {
        tick_at(ms);
    }
    release_at(10200);
    press(90000000);
    tick_at(1000);
    release_at(1500);
    int wakeups_before_ignored = host_task_wakeups;
    press(95000000);
    tick_at(3000);
    release_at(3300);
    int ignored_wakeups = host_task_wakeups - wakeups_before_ignored;
    paused = false;
    run_event_task();
    char first[sizeof seen];
    snprintf(first, sizeof first, "%s", seen);
    press(110000000);
    release_at(300);
    char d[2 * sizeof seen + 100];
    snprintf(d, sizeof d, "%s | the press after the ignored one: %s | wake-ups by the ignored press=%d", first, seen,
             ignored_wakeups);
    check("a press made while two presses wait is dropped whole and wakes nothing, and the next press is recorded",
          strcmp(first, ";;P0,H1000,H2000,H3000,H4000,H5000,H6000,H7000,H8000,H9000,H10000,R10200,"
                 "P0,H1000,R1500") == 0 && strcmp(seen, "P0;R300") == 0 && ignored_wakeups == 0, d);

    paused = true;
    int wakeups[4];
    int base = host_task_wakeups;
    press(140000000);
    wakeups[0] = host_task_wakeups - base;
    base = host_task_wakeups;
    tick_at(1000);
    wakeups[1] = host_task_wakeups - base;
    base = host_task_wakeups;
    tick_at(2000);
    wakeups[2] = host_task_wakeups - base;
    base = host_task_wakeups;
    release_at(2500);
    wakeups[3] = host_task_wakeups - base;
    paused = false;
    run_event_task();
    snprintf(d, sizeof d, "%s, wake-ups for the press, two ticks and the release=%d,%d,%d,%d", seen, wakeups[0],
             wakeups[1], wakeups[2], wakeups[3]);
    check("the press, each hold tick and the release wake the event task once each, and the task then delivers them",
          wakeups[0] == 1 && wakeups[1] == 1 && wakeups[2] == 1 && wakeups[3] == 1 &&
          strcmp(seen, ";;;P0,H1000,H2000,R2500") == 0, d);

    paused = true;
    press(150000000);
    release_at(1500);
    host_now_us = pressed_at + 4000 * 1000;
    hold_timer_callback(NULL);
    paused = false;
    run_event_task();
    check("a hold tick after the release adds no hold seconds", strcmp(seen, ";P0,H1000,R1500") == 0, seen);

    press(120000000);
    refuse(HAL_BUTTON_EVENT_HOLD, 3000, 2);
    hold_ticks_during_delay = 1;
    host_on_delay = tick_while_task_waits;
    tick_at(3050);
    int64_t waited_us = host_now_us - (pressed_at + 3050 * 1000);
    host_on_delay = NULL;
    tick_at(3150);
    release_at(3500);
    snprintf(d, sizeof d, "%s, waited %lld ms after 2 refusals", seen, (long long)(waited_us / 1000));
    check("a 3 s hold the application refuses twice is offered again after 25 ms each, even when a hold tick "
          "arrives meanwhile, once accepted, before the later events",
          strcmp(seen, "P0;H1000,H2000,H3000;;R3500") == 0 && waited_us == 2 * RETRY_MS * 1000, d);

    press(130000000);
    refuse(HAL_BUTTON_EVENT_RELEASE, 600, 1);
    release_at(600);
    snprintf(d, sizeof d, "%s, hold timer armed after the release=%d", seen, host_timer_named("btn_hold")->armed);
    check("a release the application refuses after the hold timer stopped is delivered on the retry",
          strcmp(seen, "P0;R600") == 0 && !host_timer_named("btn_hold")->armed, d);
    return check_summary();
}
