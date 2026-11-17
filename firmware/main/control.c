/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * control.c - display side of the optional control channel (protocol v3).
 *
 * The pairing key comes from NVS and is never logged or sent anywhere. Only
 * MACs computed with it leave the device. Plain C + cJSON + mbedTLS, so it
 * is tested on a PC against vectors produced by Python (tests/test_control.c).
 */
#include "control.h"
#include "cJSON.h"
#include "mbedtls/md.h"
#include <string.h>
#include <stdio.h>

bool panel_key_valid(const char *key)
{
    return key && strlen(key) == 64 && strspn(key, "0123456789abcdef") == 64;
}

/* HMAC-SHA256(key, body) written as 64 lower case hex chars + NUL.
 *
 * key is 64 hex chars that stand for 32 raw bytes. They are converted two
 * characters at a time: "a3" -> 0xa3. The raw key only lives in a stack
 * buffer for the duration of the call and is wiped afterwards, so it isn't
 * left lying around in RAM after a crash dump or a stack overflow.
 *
 * Output: each digest byte becomes two hex digits ("%02x"), so 32 bytes
 * become 64 chars. mac + i*2 is where byte i goes. */
static bool sign(const char *body, const char *key, char mac[65])
{
    if (!panel_key_valid(key)) return false;
    uint8_t bytes[32], digest[32];
    for (int i = 0; i < 32; ++i) {
        char pair[] = {key[2*i], key[2*i+1], 0}; unsigned value;
        if (sscanf(pair, "%x", &value) != 1) return false;
        bytes[i] = value;
    }
    if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), bytes, 32,
                       (const unsigned char *)body, strlen(body), digest) != 0) return false;
    memset(bytes, 0, sizeof bytes);
    for (int i = 0; i < 32; ++i) snprintf(mac + i*2, 3, "%02x", digest[i]);
    return true;
}

/* Signed string (same order as host/panel_wifi/control.py canonical()):
 *   3|command|machine|screen|session|nonce|id|token|action|rpm|axis|wcs
 * None of the fields can contain '|' (they are all checked formats), so the
 * concatenation is unambiguous. */
int panel_command_encode(char *out, size_t capacity, const panel_command_t *c,
    const panel_sample_t *s, const char *machine, const char *screen,
    const char *nonce, const char *id, const char *key)
{
    char body[256], mac[65];
    int n = snprintf(body, sizeof body, "3|command|%s|%s|%s|%s|%s|%s|%s|%d|%s|%d",
        machine, screen, s->session, nonce, id, s->token, c->action, c->rpm, c->axis, c->wcs);
    if (n < 0 || (size_t)n >= sizeof body || !sign(body, key, mac)) return 0;
    n = snprintf(out, capacity,
        "{\"v\":3,\"type\":\"command\",\"machine\":\"%s\",\"screen\":\"%s\",\"session\":\"%s\",\"nonce\":\"%s\",\"id\":\"%s\",\"token\":\"%s\",\"action\":\"%s\",\"rpm\":%d,\"axis\":\"%s\",\"wcs\":%d,\"mac\":\"%s\"}",
        machine, screen, s->session, nonce, id, s->token, c->action, c->rpm, c->axis, c->wcs, mac);
    return n > 0 && (size_t)n < capacity ? n : 0;
}

/* String member of a JSON object, or "" if missing / not a string. Returning
 * "" instead of NULL lets the caller chain strcmp() calls safely. */
static const char *field(cJSON *root, const char *name)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(value) ? value->valuestring : "";
}

bool panel_ack_decode(const char *json, size_t length, const char *machine, const char *screen,
    const char *session, const char *nonce, const char *id, const char *key, char result[24])
{
    if (!length || length > 1200 || memchr(json, 0, length)) return false;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    if (!root) return false;
    while (end < json + length && strchr(" \r\n\t", *end)) ++end;
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "v");
    const char *status = field(root, "result"), *received = field(root, "mac");
    bool ok = end == json + length && cJSON_IsObject(root) && cJSON_IsNumber(version) && version->valuedouble == 3
        && strcmp(field(root,"type"),"ack") == 0 && strcmp(field(root,"machine"),machine) == 0
        && strcmp(field(root,"screen"),screen) == 0 && strcmp(field(root,"session"),session) == 0
        && strcmp(field(root,"nonce"),nonce) == 0 && strcmp(field(root,"id"),id) == 0
        && strlen(status) > 0 && strlen(status) < 24 && strspn(status,"abcdefghijklmnopqrstuvwxyz_") == strlen(status)
        && strlen(received) == 64;
    char body[256], expected[65];
    if (ok) {
        int n = snprintf(body,sizeof body,"3|ack|%s|%s|%s|%s|%s|%s",machine,screen,session,nonce,id,status);
        ok = n > 0 && (size_t)n < sizeof body && sign(body,key,expected);
        /* Constant-time comparison. memcmp/strcmp stop at the first
         * difference, so the time taken leaks how many leading characters
         * of a forged MAC were right, and an attacker can guess it one
         * character at a time. XOR is 0 only for equal bytes; OR-ing all
         * XORs together gives 0 only if every byte matched, and the loop
         * always runs the full 64 iterations. */
        unsigned difference = 0;
        if (ok) for (int i=0;i<64;++i) difference |= expected[i] ^ received[i];
        ok = ok && difference == 0;
        if (ok) strcpy(result,status);
    }
    cJSON_Delete(root); return ok;
}
