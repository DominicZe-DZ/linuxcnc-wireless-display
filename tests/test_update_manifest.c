/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * OTA manifest: pairing key signature, bounded fields, tampering rejected.
 * Every case signs its own body, so a rejection comes from the field under
 * test and not from a signature that no longer matched. */
#include "update.h"
#include <mbedtls/md.h>
#include <stdio.h>
#include <string.h>

static int fails;

static void check(const char *name, bool ok)
{
    printf("%s: %s\n", ok ? "OK " : "FAIL", name);
    if (!ok) ++fails;
}

static void hmac_body(const char *body, const char *key, char mac[65])
{
    uint8_t bytes[32], digest[32];
    for (int i = 0; i < 32; ++i) {
        char pair[] = {key[2*i], key[2*i+1], 0};
        unsigned value;
        sscanf(pair, "%x", &value);
        bytes[i] = (uint8_t)value;
    }
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), bytes, 32,
                    (const unsigned char *)body, strlen(body), digest);
    for (int i = 0; i < 32; ++i) snprintf(mac + i*2, 3, "%02x", digest[i]);
}

/* Correctly signed manifest for that version, size and nonce. */
static void manifest(char *json, size_t capacity, const char *version,
                     unsigned size, const char *sha, const char *nonce,
                     const char *key)
{
    char body[192], mac[65];
    snprintf(body, sizeof body, "fw|%s|%u|%s|%s", version, size, sha, nonce);
    hmac_body(body, key, mac);
    snprintf(json, capacity,
        "{\"version\":\"%s\",\"size\":%u,\"sha256\":\"%s\",\"mac\":\"%s\"}",
        version, size, sha, mac);
}

int main(void)
{
    const char *key = "abababababababababababababababababababababababababababababababab";
    const char *sha = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    const char *nonce = "fedcba0987654321";
    char json[320];
    panel_upd_t upd;

    manifest(json, sizeof json, "29025aa", 1258432u, sha, nonce, key);
    check("valid manifest", panel_upd_parse(json, strlen(json), key, nonce, &upd));
    check("version parsed", strcmp(upd.version, "29025aa") == 0);
    check("size parsed", upd.size == 1258432);
    check("sha parsed", strcmp(upd.sha256, sha) == 0);

    char mac_roto[65], roto[320];
    manifest(roto, sizeof roto, "29025aa", 1258432u, sha, nonce, key);
    const char *mac_field = strstr(roto, "\"mac\":\"");
    snprintf(mac_roto, sizeof mac_roto, "%s", mac_field + 7);
    mac_roto[3] = mac_roto[3] == 'a' ? 'b' : 'a';
    snprintf(json, sizeof json,
        "{\"version\":\"29025aa\",\"size\":1258432,\"sha256\":\"%s\",\"mac\":\"%s\"}",
        sha, mac_roto);
    check("rechaza mac manipulada", !panel_upd_parse(json, strlen(json), key, nonce, &upd));

    /* Each field rejection happens with a correct signature for that body. */
    manifest(json, sizeof json, "29025aa", 1023u, sha, nonce, key);
    check("rejects size below minimum", !panel_upd_parse(json, strlen(json), key, nonce, &upd));
    manifest(json, sizeof json, "29025aa", 3u * 1024u * 1024u + 1u, sha, nonce, key);
    check("rejects size above partition", !panel_upd_parse(json, strlen(json), key, nonce, &upd));

    manifest(json, sizeof json, "0123456789abcde", 1258432u, sha, nonce, key);
    check("accepts 15 char version", panel_upd_parse(json, strlen(json), key, nonce, &upd));
    manifest(json, sizeof json, "0123456789abcdef", 1258432u, sha, nonce, key);
    check("rejects 16 char version", !panel_upd_parse(json, strlen(json), key, nonce, &upd));

    manifest(json, sizeof json, "29025aa", 1258432u, sha, nonce, key);
    check("rechaza nonce ajeno", !panel_upd_parse(json, strlen(json), key,
          "0123456789abcdef", &upd));

    const char *key_mala = "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd";
    check("rejects a different pairing key", !panel_upd_parse(json, strlen(json),
          key_mala, nonce, &upd));

    /* upper case sha: the contract wants lower case hex */
    snprintf(json, sizeof json,
        "{\"version\":\"29025aa\",\"size\":1258432,\"sha256\":\"0011223344556677"
        "8899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF\",\"mac\":\"%s\"}",
        mac_roto);
    check("rejects upper case sha", !panel_upd_parse(json, strlen(json), key, nonce, &upd));

    manifest(json, sizeof json, "", 1258432u, sha, nonce, key);
    check("rejects empty version", !panel_upd_parse(json, strlen(json), key, nonce, &upd));

    manifest(json, sizeof json, "29025aa", 1258432u, sha, nonce, key);
    char basura[340];
    snprintf(basura, sizeof basura, "%s{}", json);
    check("rechaza documento doble", !panel_upd_parse(basura, strlen(basura), key, nonce, &upd));

    check("rechaza json basura", !panel_upd_parse("{\"version\":", 12, key, nonce, &upd));
    check("rechaza puntero nulo", !panel_upd_parse(NULL, 0, key, nonce, &upd));
    check("rechaza nonce mal formado", !panel_upd_parse(json, strlen(json), key,
          "FEDCBA0987654321", &upd));

    printf(fails ? "FAILURES: %d\n" : "all OK\n", fails);
    return fails ? 1 : 0;
}
