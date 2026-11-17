/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * Tests for the battery estimator that also runs on the ESP32. */
#include "battery.h"
#include <assert.h>
#include <stdio.h>

static int64_t t;

/* Feeds N seconds at a fixed voltage and returns the last status. */
static battery_status_t run(battery_model_t *m, int seconds, float volts)
{
    for (int i = 0; i < seconds; ++i, t += 1000) battery_model_feed(m, t, volts);
    return m->status;
}

int main(void)
{
    assert(battery_percent_from_volts(4.25f) == 100);
    assert(battery_percent_from_volts(4.20f) == 100);
    assert(battery_percent_from_volts(3.84f) == 50);
    assert(battery_percent_from_volts(3.73f) == 20);
    assert(battery_percent_from_volts(3.20f) == 0);
    for (float v = 3.0f; v < 4.3f; v += 0.01f)
        assert(battery_percent_from_volts(v) <= battery_percent_from_volts(v + 0.01f));

    battery_model_t m;
    battery_model_init(&m);
    assert(!m.status.valid);
    /* On battery, steady: valid, not charging. */
    battery_status_t s = run(&m, 60, 3.85f);
    assert(s.valid && !s.charging && s.percent == 55);
    /* +-10 mV of noise is not charging. */
    for (int i = 0; i < 400; ++i, t += 1000) battery_model_feed(&m, t, 3.85f + (i % 2 ? 0.01f : -0.01f));
    assert(!m.status.charging);
    /* Normal slow discharge: still not charging. */
    for (int i = 0; i < 600; ++i, t += 1000) battery_model_feed(&m, t, 3.85f - i * 0.00005f);
    assert(!m.status.charging);
    /* Plug in: +0.10 V jump. */
    s = run(&m, 12, 3.92f);
    assert(s.charging);
    /* Slow charge on the plateau: stays charging. */
    for (int i = 0; i < 900; ++i, t += 1000) battery_model_feed(&m, t, 3.92f + i * 0.00004f);
    assert(m.status.charging);
    /* Unplug: 0.10 V drop. */
    s = run(&m, 12, 3.86f);
    assert(!s.charging);
    s = run(&m, 400, 3.86f);
    assert(!s.charging);

    /* Boot already plugged in and full (USB holds ~4.2 V). */
    battery_model_init(&m);
    s = run(&m, 3, 4.20f);
    assert(s.charging && s.percent == 100);
    /* Unplug when full: drops to 4.12 V with the display on. */
    s = run(&m, 15, 4.12f);
    assert(!s.charging);
    /* Slow cell recovery after unplugging is not charging. */
    for (int i = 0; i < 120; ++i, t += 1000) battery_model_feed(&m, t, 4.12f + i * 0.0003f);
    assert(!m.status.charging);

    /* Boot plugged in at half charge: the slow trend catches it. */
    battery_model_init(&m);
    for (int i = 0; i < 400; ++i, t += 1000) battery_model_feed(&m, t, 3.80f + i * 0.0001f);
    assert(m.status.charging);

    /* Out of range reading: no data. */
    assert(battery_model_feed(&m, t, 0.2f));
    assert(!m.status.valid && !m.status.charging);
    printf("battery: curve, noise, plug/unplug and boot on USB OK\n");
    return 0;
}
