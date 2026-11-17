/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * telemetry.c - parser for protocol v2 snapshots (PC -> display).
 *
 * Telemetry only: nothing in here can make the machine do anything. The
 * parser is plain C + cJSON with no ESP-IDF dependency, so the very same file
 * is compiled and tested on a PC (tests/test_telemetry.c).
 *
 * Design rule: all-or-nothing. Everything is decoded into a local copy and
 * only committed to *out at the very end if no check failed.
 */
#include "telemetry.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* Largest magnitude accepted for any number (mm, RPM, mm/s). Real values are
 * far below; anything bigger is a bug or garbage, not a measurement. */
#define MAX_ABS_VALUE 1e7

/* Serial number arithmetic for a wrapping 32-bit counter (same idea as
 * RFC 1982, used by TCP for its sequence numbers).
 *
 * The PC counts 0, 1, 2 ... 4294967295, 0, 1 ... A plain "candidate >
 * previous" would reject every packet right after the wrap. Instead we look
 * at the unsigned difference, which C computes modulo 2^32:
 *
 *   previous = 0xFFFFFFFE, candidate = 0x00000001
 *   delta = 1 - 0xFFFFFFFE = 3 (mod 2^32)          -> newer
 *
 *   previous = 10, candidate = 7
 *   delta = 7 - 10 = 0xFFFFFFFD (mod 2^32)         -> older (top bit set)
 *
 * Anything up to 2^31 - 1 ahead counts as newer, which at 25 packets per
 * second is about 2.7 years of packets: plenty. delta == 0 is a duplicate. */
bool panel_sequence_newer(uint32_t candidate, uint32_t previous)
{
    uint32_t delta = candidate - previous;
    return delta != 0 && delta < UINT32_C(0x80000000);
}

/* True for exactly 16 lower case hex digits ([0-9a-f]{16}). Used for nonce,
 * session and token, which are 64-bit random numbers written in hex. Upper
 * case is rejected on purpose so there is only one valid spelling of each
 * value and string comparisons are enough. */
static bool hex16(const char *value)
{
    if (!value || strlen(value) != 16) return false;
    for (int i = 0; i < 16; ++i)
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f'))) return false;
    return true;
}

/* True if the cJSON number holds an exact integer in [low, high]. JSON has
 * only doubles, so "2.5" or "1e400" must be caught here before any cast. */
static bool integer_in(const cJSON *item, double low, double high)
{
    return cJSON_IsNumber(item) && isfinite(item->valuedouble)
        && item->valuedouble >= low && item->valuedouble <= high
        && floor(item->valuedouble) == item->valuedouble;
}

/* True for a string of lower case hex digits of exactly `length` chars. */
static bool hex_string(const cJSON *item, size_t length)
{
    return cJSON_IsString(item) && strlen(item->valuestring) == length
        && strspn(item->valuestring, "0123456789abcdef") == length;
}

bool panel_decode(const char *json, size_t length, const char *nonce,
                  const char *machine, const char *screen, panel_sample_t *out)
{
    /* 1200 is the protocol limit (keeps every datagram under one Ethernet
     * MTU). An embedded NUL would make cJSON stop early and accept a prefix. */
    if (length == 0 || length > 1200 || memchr(json, '\0', length)) return false;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    if (!root) return false;

    /* cJSON stops after the first complete value. Skip trailing whitespace
     * and require that nothing else follows: "{...} garbage" or two JSON
     * documents glued together are rejected. */
    while (end < json + length && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) ++end;
    bool ok = end == json + length && cJSON_IsObject(root);

    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "v");
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *challenge = cJSON_GetObjectItemCaseSensitive(root, "nonce");
    cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session");
    cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
    cJSON *valid = cJSON_GetObjectItemCaseSensitive(root, "valid");
    cJSON *demo = cJSON_GetObjectItemCaseSensitive(root, "demo");
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON *machine_id = cJSON_GetObjectItemCaseSensitive(root, "machine");
    cJSON *screen_id = cJSON_GetObjectItemCaseSensitive(root, "screen");

    /* Envelope: right version and type, addressed to THIS machine/screen
     * pair, answering OUR current nonce (stale packets from an old session
     * fail here), and a sequence number that fits in uint32_t. */
    ok = ok && cJSON_IsNumber(v) && v->valuedouble == 2 && cJSON_IsString(type)
        && cJSON_IsString(machine_id) && strcmp(machine_id->valuestring, machine) == 0
        && cJSON_IsString(screen_id) && strcmp(screen_id->valuestring, screen) == 0
        && strcmp(type->valuestring, "sample") == 0 && cJSON_IsString(challenge)
        && hex16(challenge->valuestring) && strcmp(challenge->valuestring, nonce) == 0
        && cJSON_IsString(session) && hex16(session->valuestring)
        && integer_in(seq, 0, 4294967295.0)
        && cJSON_IsBool(valid) && cJSON_IsBool(demo) && cJSON_IsObject(data);

    panel_sample_t next = {0};
    if (ok) {
        next.seq = (uint32_t)seq->valuedouble;
        strcpy(next.session, session->valuestring);    /* length checked by hex16 */
        next.valid = cJSON_IsTrue(valid);
        next.demo = cJSON_IsTrue(demo);

        /* Numeric channels, bits 0..4 of `present`. Three cases per field:
         *   number in range  -> store it and set its presence bit
         *   null or missing  -> leave it at 0 with the bit clear ("unknown")
         *   anything else    -> malformed packet, reject everything */
        const char *names[] = {"rpm", "feed", "x", "y", "z"};
        float *fields[] = {&next.rpm, &next.feed, &next.x, &next.y, &next.z};
        for (int i = 0; i < 5; ++i) {
            cJSON *item = cJSON_GetObjectItemCaseSensitive(data, names[i]);
            if (cJSON_IsNumber(item) && isfinite(item->valuedouble) && fabs(item->valuedouble) <= MAX_ABS_VALUE) {
                *fields[i] = (float)item->valuedouble;
                next.present |= PANEL_BIT(PANEL_CH_RPM + i);
            } else if (item && !cJSON_IsNull(item)) ok = false;
        }

        /* Boolean channels, bits 5..6, same three cases. */
        const char *bits[] = {"spindle_on", "coolant"};
        bool *flags[] = {&next.spindle_on, &next.coolant};
        for (int i = 0; i < 2; ++i) {
            cJSON *item = cJSON_GetObjectItemCaseSensitive(data, bits[i]);
            if (cJSON_IsBool(item)) {
                *flags[i] = cJSON_IsTrue(item);
                next.present |= PANEL_BIT(PANEL_CH_SPINDLE_ON + i);
            } else if (item && !cJSON_IsNull(item)) ok = false;
        }

        /* Optional "ui" object (DRO / Spindle pages). If it is present it
         * must be complete and sane; if it is absent the sample is still
         * valid and ui_available stays false (pages stay locked). */
        cJSON *ui = cJSON_GetObjectItemCaseSensitive(root, "ui");
        if (ui) {
            cJSON *state = cJSON_GetObjectItemCaseSensitive(ui, "state");
            cJSON *token = cJSON_GetObjectItemCaseSensitive(ui, "token");
            cJSON *allowed = cJSON_GetObjectItemCaseSensitive(ui, "allowed");
            cJSON *wcs = cJSON_GetObjectItemCaseSensitive(ui, "wcs");
            cJSON *selected = cJSON_GetObjectItemCaseSensitive(ui, "selected");
            cJSON *work = cJSON_GetObjectItemCaseSensitive(ui, "work");
            /* state: short word of [a-z_] so it fits state[16] and can be
             * compared with strcmp. wcs: 1..9. selected: 0..5000 RPM. */
            if (!cJSON_IsObject(ui) || !cJSON_IsString(state) || strlen(state->valuestring) > 15
                || strspn(state->valuestring, "abcdefghijklmnopqrstuvwxyz_") != strlen(state->valuestring)
                || !cJSON_IsString(token) || !hex16(token->valuestring) || !cJSON_IsBool(allowed)
                || !integer_in(wcs, 1, 9) || !integer_in(selected, 0, 5000)) ok = false;
            else {
                next.ui_available = true;
                /* Belt and braces: even if the PC said allowed=true, only
                 * "manual" can enable the controls on this side. */
                next.control_allowed = cJSON_IsTrue(allowed) && strcmp(state->valuestring, "manual") == 0;
                strcpy(next.state, state->valuestring); strcpy(next.token, token->valuestring);
                next.wcs = wcs->valueint; next.selected_rpm = selected->valueint;
                snprintf(next.nonce, sizeof next.nonce, "%s", nonce);

                /* Optional firmware offer. Only its shape is checked here;
                 * the HMAC is verified later in update_manifest.c. A broken
                 * offer just means "no offer", it must never cost us the
                 * telemetry or the controls. */
                cJSON *upd = cJSON_GetObjectItemCaseSensitive(ui, "upd");
                if (upd) {
                    cJSON *uv = cJSON_GetObjectItemCaseSensitive(upd, "version");
                    cJSON *us = cJSON_GetObjectItemCaseSensitive(upd, "size");
                    cJSON *uh = cJSON_GetObjectItemCaseSensitive(upd, "sha256");
                    cJSON *um = cJSON_GetObjectItemCaseSensitive(upd, "mac");
                    if (cJSON_IsString(uv) && uv->valuestring[0]
                        && strlen(uv->valuestring) < sizeof next.upd_version
                        && integer_in(us, 1024, 3.0 * 1024 * 1024)
                        && hex_string(uh, 64) && hex_string(um, 64)) {
                        next.upd_present = true;
                        snprintf(next.upd_version, sizeof next.upd_version, "%s", uv->valuestring);
                        next.upd_size = (uint32_t)us->valuedouble;
                        strcpy(next.upd_sha, uh->valuestring);
                        strcpy(next.upd_mac, um->valuestring);
                    } else {
                        next.upd_present = false;
                    }
                }

                /* Work coordinates: [x, y, z] or null when the PC can't
                 * compute them reliably (NML not available). */
                if (cJSON_IsArray(work) && cJSON_GetArraySize(work) == 3) {
                    next.work_present = true;
                    for (int i = 0; i < 3; ++i) {
                        cJSON *value = cJSON_GetArrayItem(work, i);
                        if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || fabs(value->valuedouble) > MAX_ABS_VALUE) ok = false;
                        else next.work[i] = value->valuedouble;
                    }
                } else if (work && !cJSON_IsNull(work)) ok = false;
            }
        }
    }
    if (ok) *out = next;    /* commit only a fully valid sample */
    cJSON_Delete(root);
    return ok;
}
