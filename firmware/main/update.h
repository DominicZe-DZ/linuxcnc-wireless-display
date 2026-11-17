/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * Signed OTA update. Downloads the image from the host over HTTP, checks the
 * manifest signature (pairing key) and the SHA-256, writes the inactive slot
 * and switches the boot partition. Everything runs in small non-blocking
 * steps from the main loop. */
#ifndef PANEL_UPDATE_H
#define PANEL_UPDATE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PANEL_UPD_SHA_LEN 64

typedef struct {
    char version[16];   /* PANEL_BUILD_ID of the offered image (<=15 + NUL) */
    uint32_t size;
    char sha256[PANEL_UPD_SHA_LEN + 1];
} panel_upd_t;

/* Progress state shown on screen. */
typedef enum {
    PANEL_UPD_IDLE = 0,
    PANEL_UPD_CONNECT,
    PANEL_UPD_DOWNLOAD,
    PANEL_UPD_VERIFY,
    PANEL_UPD_DONE,
    PANEL_UPD_FAIL
} panel_upd_state_t;

/* Parses and checks the ui.upd object (JSON, at most 512 bytes):
 * mac = HMAC-SHA256 with the pairing key over "fw|version|size|sha256|nonce"
 * (bound to the current nonce, so a captured manifest is useless in another
 * session), version up to 15 printable chars, size an integer in
 * [1024, 3 MB], sha256 and mac 64 lower case hex chars. False on any mismatch. */
bool panel_upd_parse(const char *json, size_t length, const char *control_key,
                     const char *nonce, panel_upd_t *out);

/* Starts the download from the host IP (the one sending the current stream).
 * The key isn't needed any more, the manifest was already verified. */
bool panel_upd_start(const char *host_ip, uint16_t port, const panel_upd_t *upd);

/* Runs one non-blocking step (socket + esp_ota). Percent is -1 before the
 * download starts, 0..100 while running and 100 when done. */
panel_upd_state_t panel_upd_poll(int *percent);

/* Cancels and cleans up. After a failure it goes back to IDLE so it can retry. */
void panel_upd_abort(void);

#endif
