/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * control.h - display side of the optional control channel (protocol v3).
 *
 * Builds signed command datagrams and checks signed ACKs. The signature is
 * HMAC-SHA256 with the 32-byte pairing key stored in NVS. See
 * docs/CONTROL_CHANNEL.md for the protocol and docs/design for the reasons.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "telemetry.h"

/* One command as queued by pages.c.
 *   action  "start", "stop", "speed" or "zero"
 *   axis    "x", "y", "z", "xyz" for zero, "-" otherwise
 *   rpm     0..5000 (ignored by the PC for zero)
 *   wcs     work system the user saw when confirming (1..9) */
typedef struct {char action[8], axis[4]; int rpm, wcs;} panel_command_t;

/* true if key is exactly 64 lower case hex characters (32 bytes). */
bool panel_key_valid(const char *key);

/* Writes the signed v3 command JSON into out (NUL terminated).
 * The MAC covers a fixed ordered list of ASCII fields joined with '|', never
 * the JSON text itself: JSON key order and spacing are not guaranteed, a
 * fixed string is. Returns the JSON length, or 0 if it didn't fit or the
 * key is invalid. */
int panel_command_encode(char *out, size_t capacity, const panel_command_t *command,
    const panel_sample_t *sample, const char *machine, const char *screen,
    const char *nonce, const char *id, const char *key);

/* Checks that json is a v3 ACK for exactly this machine/screen/session/
 * nonce/id with a correct MAC. On success copies the result word (at most
 * 23 chars, [a-z_]) into result and returns true. */
bool panel_ack_decode(const char *json, size_t length, const char *machine, const char *screen,
    const char *session, const char *nonce, const char *id, const char *key, char result[24]);
