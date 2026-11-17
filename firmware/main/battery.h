/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 */
#pragma once
/* Optional LiPo cell on the Waveshare ESP32-S3-Touch-LCD-2.8C.
 *
 * The board only exposes the cell voltage (BAT_ADC on GPIO4, 200k/100k
 * divider). The charger status pin is not wired, so "charging" is a guess
 * based on how the voltage moves. It can't tell whether USB is plugged in,
 * whether a cell is present or how much current flows. See docs/BATTERY.md.
 */
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool valid;     /* reading within the LiPo range */
    bool charging;  /* estimated, there is no charger signal */
    int percent;    /* 0..100, rough estimate from voltage */
    float volts;    /* filtered cell voltage */
} battery_status_t;

/* Pure logic, no hardware access, so it can be unit tested on a PC. */
#define BATTERY_HISTORY 31 /* 300 s, one sample every 10 s */
typedef struct {
    float filtered;                 /* low-pass filtered voltage, V */
    float history[BATTERY_HISTORY]; /* one per 10 s, history[0] is the newest */
    float recent[11];               /* one per call, recent[10] = 10 s ago */
    int samples;                    /* calls since the last reset */
    int history_count;              /* valid entries in history[] */
    int64_t last_ms;                /* time of the last call */
    int64_t history_ms;             /* time history[0] was stored */
    int64_t hold_until_ms;          /* ignore the 4.17 V rule until then */
    battery_status_t status;        /* last result */
} battery_model_t;

/* 0..100 from a resting voltage, interpolated on a typical LiPo curve. */
int battery_percent_from_volts(float volts);
/* Clears everything (also used when the reading goes out of range). */
void battery_model_init(battery_model_t *model);
/* Feed roughly one sample per second. Returns true when the status changed. */
bool battery_model_feed(battery_model_t *model, int64_t now_ms, float volts);

/* Hardware side (ESP32 only). battery_poll reads at most once per second and
 * returns true when the visible status changed. */
void battery_init(void);
bool battery_poll(int64_t now_ms, battery_status_t *out);
