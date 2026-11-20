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

static void on_button(hal_button_event_t event, uint32_t hold_ms)
{
}

static void drain(void)
{
    button_msg_t m;
    bool first = true;
    while (xQueueReceive(s_button.queue, &m, 0) == pdTRUE) {
        const char *n = m.event == HAL_BUTTON_EVENT_PRESS ? "P" : m.event == HAL_BUTTON_EVENT_HOLD ? "H" : "R";
        snprintf(seen + strlen(seen), sizeof seen - strlen(seen), "%s%s%u", first ? "" : ",", n, (unsigned)m.hold_ms);
        first = false;
    }
}

static void edge(int level)
{
    host_gpio_level = level;
    host_gpio_isr(NULL);
    host_timer_fire(host_timer_named("btn_debounce"));
    drain();
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
    drain();
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
    return check_summary();
}
