/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * dashboard.h - Classic page (RPM gauge) and the shared battery icon.
 * All functions must be called from the LVGL thread (the main loop).
 */
#pragma once
#include "telemetry.h"
#include "battery.h"

/* Builds the Classic page and then the DRO/Spindle pages (pages_init).
 * Call once after LVGL_Init(). */
void dashboard_init(void);

/* Shows the latest sample on every page.
 *   sample  last accepted sample (contents ignored when fresh is false)
 *   fresh   a valid sample arrived within PANEL_STALE_MS
 *   wifi    associated to the AP and holding an IP address */
void dashboard_update(const panel_sample_t *sample, bool fresh, bool wifi);

/* Updates the battery icon. No-op when nothing visible changed, so nothing
 * gets invalidated. Compiles to nothing with PANEL_BATTERY=n. */
void dashboard_battery(const battery_status_t *battery);
