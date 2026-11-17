/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * pages.h - DRO and Spindle pages, page navigation, the zero dialog, the
 * lock screen and the OTA progress screen.
 *
 * pages.c never talks to the network. It queues at most ONE command
 * (pages_take_command) and main.c sends it; the answer comes back through
 * pages_result(). All functions run on the LVGL thread.
 */
#pragma once
#include "lvgl.h"
#include "control.h"
#include "update.h"

/* Creates the DRO (view 1) and Spindle (view 2) pages. `classic` is the
 * Classic page container (view 0) and `classic_logo` its DominicZe logo,
 * which becomes the "next page" button. */
void pages_init(lv_obj_t *classic, lv_obj_t *classic_logo);

/* New sample for the DRO/Spindle pages (same meaning as dashboard_update). */
void pages_update(const panel_sample_t *sample, bool fresh, bool wifi);

/* true when a valid pairing key is stored in NVS (controls can be enabled),
 * false while there is no key or an OTA update is running. */
void pages_control_ready(bool ready);

/* Hands the queued command, if any, to main.c. Returns true and fills *out
 * once per command; the queue is then empty. */
bool pages_take_command(panel_command_t *out);

/* Result of the last command: an ACK result word from the PC ("ok",
 * "busy", "program"...) or a local one ("timeout", "dropped", "error").
 * "pending" is ignored: the final answer is still to come. */
void pages_result(const char *result);

/* Current page: 0 Classic, 1 DRO, 2 Spindle. */
int pages_view(void);

/* true while the light lock screen covers the Spindle page (the battery
 * icon switches to dark ink so it stays visible). */
bool pages_white(void);

/* Shows the OTA progress screen (state and 0..100 percent) and locks every
 * control. pages_ota_end() restores the previous page after a failure. */
void pages_ota(panel_upd_state_t state, int percent);
void pages_ota_end(void);
