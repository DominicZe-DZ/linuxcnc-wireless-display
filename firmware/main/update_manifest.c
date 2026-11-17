/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * Update manifest parser. No hardware dependencies, so CI tests it with gcc.
 * Manifest: {"version","size","sha256","mac"}, mac = HMAC-SHA256 with the
 * 32 byte pairing key over "fw|version|size|sha256|nonce". size is always
 * an integer, the host writes it with json.dumps(int). */
#include "update.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "mbedtls/md.h"

#define PANEL_UPD_JSON_MAX 512

/* true for exactly 64 lower case hex chars (a SHA-256 or an HMAC). */
static bool hex64(const char *text)
{
    return text && strlen(text) == PANEL_UPD_SHA_LEN
        && strspn(text, "0123456789abcdef") == PANEL_UPD_SHA_LEN;
}

/* HMAC-SHA256(key, body) as 64 hex chars. Same conversion as sign() in
 * control.c: the 64 hex chars of the key are turned into 32 raw bytes two
 * characters at a time ("a3" -> 0xa3), and the 32 digest bytes back into 64
 * hex chars. The raw key is wiped from the stack before returning. */
static bool hmac_fw(const char *body, const char *key, char mac[65])
{
    if (!key || strlen(key) != 64 || strspn(key, "0123456789abcdef") != 64)
        return false;
    uint8_t bytes[32], digest[32];
    for (int i = 0; i < 32; ++i) {
        char pair[] = {key[2*i], key[2*i+1], 0};
        unsigned value;
        if (sscanf(pair, "%x", &value) != 1) return false;
        bytes[i] = (uint8_t)value;
    }
    if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), bytes, 32,
                        (const unsigned char *)body, strlen(body), digest) != 0) {
        memset(bytes, 0, sizeof bytes);
        return false;
    }
    memset(bytes, 0, sizeof bytes);
    for (int i = 0; i < 32; ++i) snprintf(mac + i*2, 3, "%02x", digest[i]);
    return true;
}

/* See update.h. The signed text is rebuilt here from the parsed values,
 * in the same format the PC used (app.py / firmware_server.py):
 *   fw|<version>|<size as integer>|<sha256>|<nonce>
 * so it doesn't matter how the JSON was spaced or ordered. */
bool panel_upd_parse(const char *json, size_t length, const char *control_key,
                     const char *nonce, panel_upd_t *out)
{
    if (!json || !control_key || !out) return false;
    if (!nonce || strlen(nonce) != 16
            || strspn(nonce, "0123456789abcdef") != 16)
        return false;
    memset(out, 0, sizeof *out);
    if (length == 0 || length > PANEL_UPD_JSON_MAX || memchr(json, '\0', length))
        return false;

    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    if (!root) return false;
    while (end && end < json + length && strchr(" \r\n\t", *end)) ++end;

    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    cJSON *sha = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    cJSON *mac = cJSON_GetObjectItemCaseSensitive(root, "mac");

    bool ok = end == json + length && cJSON_IsObject(root)
        && cJSON_IsString(version) && version->valuestring && version->valuestring[0]
        && strlen(version->valuestring) < sizeof out->version
        && strspn(version->valuestring, "0123456789abcdefghijklmnopqrstuvwxyz")
            == strlen(version->valuestring)
        && cJSON_IsNumber(size) && isfinite(size->valuedouble)
        && size->valuedouble >= 1024.0 && size->valuedouble <= 3.0 * 1024 * 1024
        && floor(size->valuedouble) == size->valuedouble
        && cJSON_IsString(sha) && hex64(sha->valuestring)
        && cJSON_IsString(mac) && hex64(mac->valuestring);

    if (ok) {
        char body[192], expected[65];
        int n = snprintf(body, sizeof body, "fw|%s|%u|%s|%s", version->valuestring,
                         (unsigned)size->valuedouble, sha->valuestring, nonce);
        if (n <= 0 || (size_t)n >= sizeof body || !hmac_fw(body, control_key, expected)) {
            ok = false;
        } else {
            /* Constant-time comparison, see panel_ack_decode() in control.c. */
            unsigned difference = 0;
            for (int i = 0; i < 64; ++i)
                difference |= (unsigned)(expected[i] ^ mac->valuestring[i]);
            ok = difference == 0;
        }
        if (ok) {
            snprintf(out->version, sizeof out->version, "%s", version->valuestring);
            out->size = (uint32_t)size->valuedouble;
            memcpy(out->sha256, sha->valuestring, PANEL_UPD_SHA_LEN + 1);
        }
    }
    cJSON_Delete(root);
    return ok;
}
