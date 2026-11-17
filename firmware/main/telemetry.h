/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * telemetry.h - one decoded telemetry snapshot and the parser that fills it.
 *
 * The PC sends a complete JSON snapshot 25 times per second (protocol v2,
 * see docs/PROTOCOL.md). panel_decode() checks it field by field and only
 * copies it into a panel_sample_t when EVERYTHING is valid. The rest of the
 * firmware never looks at JSON, only at this structure.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Bit positions inside panel_sample_t.present.
 *
 * A channel the PC couldn't read is sent as null (or left out). That must
 * never be confused with a real zero: "X = 0.000" and "X unknown" are very
 * different things on a machine. So every channel has a presence bit:
 *
 *   bit:   6        5        4  3  2   1     0
 *        coolant spindle_on  z  y  x  feed  rpm
 *
 * Example: rpm, feed and spindle_on present -> 0b0100011 = 0x23 = 35.
 * Bit 7 is unused. Test a channel with (sample.present & PANEL_BIT_X). */
enum {
    PANEL_CH_RPM = 0,
    PANEL_CH_FEED = 1,
    PANEL_CH_X = 2,          /* X, Y, Z are consecutive: PANEL_CH_X + axis */
    PANEL_CH_Y = 3,
    PANEL_CH_Z = 4,
    PANEL_CH_SPINDLE_ON = 5,
    PANEL_CH_COOLANT = 6,
};
#define PANEL_BIT(channel)  (1u << (channel))
#define PANEL_BIT_RPM       PANEL_BIT(PANEL_CH_RPM)
#define PANEL_BIT_FEED      PANEL_BIT(PANEL_CH_FEED)

typedef struct {
    /* --- protocol v2 "data" object --------------------------------------- */
    float rpm;          /* spindle RPM, already scaled on the PC            */
    float feed;         /* current feed in mm/s (display shows x60 mm/min)  */
    float x, y, z;      /* machine position, mm                             */
    bool spindle_on;    /* on, or really turning (see app.py)               */
    bool coolant;       /* flood OR mist, combined on the PC                */
    uint8_t present;    /* presence bits, see PANEL_CH_* above              */

    /* --- envelope -------------------------------------------------------- */
    bool valid;         /* panel_wifi.enabled on the PC                     */
    bool demo;          /* PC running with --demo, data is fake             */
    uint32_t seq;       /* sequence number, wraps at 2^32                   */
    char session[17];   /* 16 hex chars + NUL, random per PC process run    */

    /* --- optional "ui" object ---------------------------------------------
     * A v2 PC without it leaves ui_available = false, which keeps every
     * control locked: no permission is ever assumed. */
    bool ui_available;
    bool control_allowed;   /* allowed == true AND state == "manual"        */
    bool work_present;      /* work[] holds valid numbers                   */
    int wcs;                /* 1 = G54 ... 6 = G59, 7..9 = G59.1..G59.3     */
    int selected_rpm;       /* speed to show on the Spindle page, 0..5000   */
    float work[3];          /* work coordinates X, Y, Z in mm               */
    char state[16];         /* LinuxCNC state word, see docs/PROTOCOL.md    */
    char token[17];         /* 1 s permission token for the next command    */

    /* --- optional firmware offer (ui.upd) -------------------------------- */
    bool upd_present;
    char upd_version[16];   /* build ID of the offered image                */
    uint32_t upd_size;      /* bytes                                        */
    char upd_sha[65];       /* SHA-256 of the image, 64 hex                 */
    char upd_mac[65];       /* HMAC of the offer, 64 hex                    */

    /* Copy of the display nonce this sample answered. The OTA signature is
     * computed over it, so the offer is only valid for this session. */
    char nonce[17];
} panel_sample_t;

/* Decodes one datagram.
 *
 *   json, length  raw UDP payload (not NUL terminated, at most 1200 bytes)
 *   nonce         the display's current nonce; the packet must echo it
 *   machine       expected machine ID (from NVS / menuconfig)
 *   screen        this display's ID (its Wi-Fi MAC as 12 hex chars)
 *   out           written ONLY when the function returns true
 *
 * Returns false for anything that isn't a well formed v2 sample for this
 * exact machine/screen/nonce. Any bad field rejects the whole packet, so the
 * screen never shows a half-updated mixture of old and new values. */
bool panel_decode(const char *json, size_t length, const char *nonce,
                  const char *machine, const char *screen, panel_sample_t *out);

/* True if sequence number `candidate` comes after `previous`, taking the
 * 32-bit wrap-around into account (see telemetry.c). */
bool panel_sequence_newer(uint32_t candidate, uint32_t previous);
