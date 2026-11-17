/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * battery_model.c - state of charge and charger state estimated from the
 * cell voltage. Pure C, no hardware: tested on a PC (tests/test_battery.c).
 *
 * The percentage follows a typical resting curve for a 3.7 V LiPo. With the
 * display on, or while charging, the measured voltage shifts, so treat it as
 * a rough indication and not as a fuel gauge.
 *
 * There is no charger signal, so "charging" is inferred like this:
 *  - Plugging in makes the voltage jump: +35 mV or more within 10 s.
 *    Unplugging drops it the same way.
 *  - If the display boots already plugged in, the 5 minute trend goes up.
 *  - Above 4.17 V we assume USB power: a cell feeding the display falls
 *    below that within a few seconds.
 */
#include "battery.h"
#include <string.h>

/* Resting voltage -> state of charge for a typical single cell LiPo, in 5 %
 * steps. Not linear: the cell sits on a long flat plateau around 3.7-3.9 V
 * and drops fast below 3.6 V, which is why 5 % at the bottom spans 0.31 V
 * and 5 % in the middle only 0.02 V. Values in between are interpolated. */
static const struct { float volts; int percent; } curve[] = {
    {3.30f, 0}, {3.61f, 5}, {3.69f, 10}, {3.71f, 15}, {3.73f, 20}, {3.75f, 25},
    {3.77f, 30}, {3.79f, 35}, {3.80f, 40}, {3.82f, 45}, {3.84f, 50}, {3.85f, 55},
    {3.87f, 60}, {3.91f, 65}, {3.95f, 70}, {3.98f, 75}, {4.02f, 80}, {4.08f, 85},
    {4.11f, 90}, {4.15f, 95}, {4.20f, 100},
};

/* Piecewise linear interpolation on the table above.
 * Find the first point i with volts <= curve[i].volts; then
 *   t = (V - V[i-1]) / (V[i] - V[i-1])            0..1 inside the segment
 *   p = P[i-1] + t * (P[i] - P[i-1])
 * and round to the nearest integer (+0.5 then truncate, p is never < 0). */
int battery_percent_from_volts(float volts)
{
    const int n = sizeof curve / sizeof curve[0];
    if (volts <= curve[0].volts) return 0;
    if (volts >= curve[n - 1].volts) return 100;
    for (int i = 1; i < n; ++i) {
        if (volts > curve[i].volts) continue;
        float t = (volts - curve[i - 1].volts) / (curve[i].volts - curve[i - 1].volts);
        float p = curve[i - 1].percent + t * (curve[i].percent - curve[i - 1].percent);
        return (int)(p + 0.5f);
    }
    return 100;
}

void battery_model_init(battery_model_t *model)
{
    memset(model, 0, sizeof *model);
}

/* Feeds one reading (about once per second). Data kept by the model:
 *
 *   filtered    smoothed voltage
 *   recent[]    last 11 filtered values, one per call: recent[0] now,
 *               recent[10] ten seconds ago -> fast "step" detector
 *   history[]   31 filtered values, one every 10 s: history[0] newest,
 *               history[30] 300 s ago -> slow "trend" detector
 *
 * Both arrays are shift registers: memmove() moves every element one place
 * towards the old end (dropping the oldest) and the new value goes into
 * index 0. With 11 and 31 floats that is cheaper and simpler than a ring
 * buffer with an index to get wrong.
 *
 * Thresholds (all on the filtered voltage):
 *   start charging  step >= +35 mV in 10 s  (plug in)
 *                   trend >= +12 mV in 300 s (booted while plugged in)
 *                   V >= 4.17 V              (sitting on USB, full)
 *   stop charging   step <= -35 mV in 10 s  (unplug)
 *                   trend <= -10 mV in 300 s and V < 4.17 V
 * Returns true when valid, charging or percent changed. */
bool battery_model_feed(battery_model_t *model, int64_t now_ms, float volts)
{
    battery_status_t before = model->status;
    /* Out of range: no cell or a bad reading. Clear the history so a jump
     * that has nothing to do with the cell isn't taken as charging. */
    if (!(volts >= 2.8f && volts <= 4.5f)) {
        battery_model_init(model);
        return before.valid;
    }
    if (model->samples == 0) {
        model->filtered = volts;
        model->history_ms = now_ms;
        model->history[0] = volts;
        model->history_count = 1;
    } else {
        /* First order low-pass (exponential moving average):
         *   f[n] = f[n-1] + a * (v - f[n-1]),  a = 0.3
         * After a step, the remaining error is (1 - a)^n = 0.7^n, which is
         * 5 % after n = ln(0.05) / ln(0.7) ~ 8.4 samples, i.e. about 9 s
         * at one sample per second. Enough to hide ADC noise and the
         * short dips when Wi-Fi transmits, fast enough to see a plug-in. */
        model->filtered += 0.3f * (volts - model->filtered);
    }
    float v = model->filtered;
    memmove(model->recent + 1, model->recent, sizeof model->recent - sizeof model->recent[0]);
    model->recent[0] = v;
    ++model->samples;
    if (now_ms - model->history_ms >= 10000) {
        memmove(model->history + 1, model->history, sizeof model->history - sizeof model->history[0]);
        model->history[0] = v;
        if (model->history_count < BATTERY_HISTORY) ++model->history_count;
        model->history_ms = now_ms;
    }
    model->last_ms = now_ms;

    /* The detectors only run once their window is full, otherwise "10 s
     * ago" or "300 s ago" would be a zero and look like a huge rise. */
    bool have10 = model->samples > 10, have300 = model->history_count == BATTERY_HISTORY;
    float step = have10 ? v - model->recent[10] : 0.0f;
    float trend = have300 ? v - model->history[BATTERY_HISTORY - 1] : 0.0f;
    bool charging = model->status.charging;
    if (charging) {
        if (step <= -0.035f || (trend <= -0.010f && v < 4.17f)) {
            charging = false;
            /* A full cell just unplugged still reads above 4.17 V for a
             * while; ignore that rule for 2 minutes or the bolt would come
             * straight back. */
            model->hold_until_ms = now_ms + 120000;
        }
    } else if (step >= 0.035f || trend >= 0.012f || (v >= 4.17f && now_ms >= model->hold_until_ms)) {
        charging = true;
    }
    if (charging && !model->status.charging) {
        /* The jump has been used. Forget it so a slow charge curve doesn't
         * trigger it again and an unplug doesn't compare with stale values. */
        for (int i = 0; i < model->history_count; ++i) model->history[i] = v;
    }
    model->status = (battery_status_t){
        .valid = true, .charging = charging,
        .percent = battery_percent_from_volts(v), .volts = v,
    };
    return before.valid != model->status.valid || before.charging != charging
        || before.percent != model->status.percent;
}
