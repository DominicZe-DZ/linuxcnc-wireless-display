/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * update.c - over-the-air update, run as a small state machine from the
 * main loop.
 *
 * HTTP/1.0 download with a mandatory Content-Length that must match the
 * signed manifest. The image is written to the inactive OTA slot with
 * esp_ota while it is hashed on the fly; the boot partition is only switched
 * if the SHA-256 matches. Nothing blocks: each poll does at most one burst of
 * socket reads (up to 8 KiB) and one flash write, so the screen keeps
 * refreshing and showing the progress.
 *
 * Phases (upd.phase) and what moves them on:
 *
 *   CONNECT --TCP connected--> REQUEST --GET sent--> HEADERS
 *      |                          |                     |
 *      |                          |          status 200, Content-Length ==
 *      |                          |          manifest size, not chunked,
 *      |                          |          not compressed
 *      |                          |                     v
 *      |                          |                   BODY --all bytes--> FINISH
 *      |                          |                     |               (hash OK,
 *      +----- any error or timeout ---------------------+--> FINISH      boot slot
 *                                                          (state FAIL)   switched,
 *                                                                         state DONE)
 *
 * Time limits: 2 s to connect, 5 s for the response header, 15 s without
 * any body data, and on average at least 6 KB/s over the whole body. */
#include "update.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/select.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "mbedtls/sha256.h"

#define UPD_BUFFER             8192   /* receive buffer, also header buffer */
#define UPD_HEADER_MAX         2048   /* longest HTTP header accepted */
#define UPD_CONNECT_TIMEOUT_MS 2000
#define UPD_HEADER_TIMEOUT_MS  5000
#define UPD_IDLE_TIMEOUT_MS    15000
#define UPD_TOTAL_MIN_RATE     6000   /* minimum sustained bytes/s */
#define UPD_MIN_BYTES          1024
#define UPD_MAX_BYTES          (3u * 1024u * 1024u)

static const char *TAG = "panel";
/* Note: EAGAIN on the non-blocking socket only means "nothing yet". An
 * early version treated it as end of file and cut downloads short. */

typedef enum {
    UPD_CONNECT = 0,
    UPD_REQUEST,
    UPD_HEADERS,
    UPD_BODY,
    UPD_FINISH
} upd_phase_t;

/* All the download state in one static block (about 8.5 KiB, mostly the
 * buffer). Static rather than heap so an update can't fail for lack of a
 * contiguous allocation after hours of running. */
static struct {
    panel_upd_state_t state;
    upd_phase_t phase;
    int sock;
    char host[16];
    uint16_t port;
    panel_upd_t manifest;
    mbedtls_sha256_context sha;
    esp_ota_handle_t ota;
    bool ota_open;
    const esp_partition_t *partition;
    uint32_t received;
    uint32_t want;
    size_t sent;
    int64_t deadline_ms;
    int64_t body_started_ms;
    uint8_t buf[UPD_BUFFER];
    size_t head_len;
} upd;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void upd_reset_socket(void)
{
    if (upd.sock >= 0) {
        close(upd.sock);
        upd.sock = -1;
    }
}

/* Single exit for every error: closes the socket, discards the half
 * written slot (esp_ota_abort leaves the running image untouched) and
 * reports FAIL. main.c then shows the message and retries in 10 minutes. */
static void upd_fail(const char *reason)
{
    ESP_LOGW(TAG, "update failed: %s", reason);
    upd_reset_socket();
    if (upd.ota_open) {
        esp_ota_abort(upd.ota);
        upd.ota_open = false;
    }
    upd.ota = 0;
    mbedtls_sha256_free(&upd.sha);
    upd.state = PANEL_UPD_FAIL;
    upd.phase = UPD_FINISH;
}

/* Writes body bytes to the inactive partition and feeds the SHA-256. */
static bool upd_body(const uint8_t *data, size_t length)
{
    if (!length) return true;
    if (upd.received + length > upd.want) {
        upd_fail("body larger than the manifest");
        return false;
    }
    if (!upd.ota_open) {
        upd.partition = esp_ota_get_next_update_partition(NULL);
        if (!upd.partition || upd.manifest.size > upd.partition->size) {
            upd_fail("OTA partition too small");
            return false;
        }
        if (esp_ota_begin(upd.partition, upd.manifest.size, &upd.ota) != ESP_OK) {
            upd.ota = 0;
            upd_fail("esp_ota_begin");
            return false;
        }
        upd.ota_open = true;
        /* esp_ota_begin erases the whole image range, which can take a few
         * seconds. Don't count that against the download rate. */
        upd.body_started_ms = now_ms();
        upd.deadline_ms = upd.body_started_ms + UPD_IDLE_TIMEOUT_MS;
    }
    if (esp_ota_write(upd.ota, data, length) != ESP_OK) {
        upd_fail("flash write");
        return false;
    }
    if (mbedtls_sha256_update(&upd.sha, data, length) != 0) {
        upd_fail("sha256");
        return false;
    }
    upd.received += (uint32_t)length;
    if (upd.received > upd.want) {
        upd_fail("body larger than the manifest");
        return false;
    }
    return true;
}

/* Hash check -> esp_ota_end -> switch boot partition. main.c restarts. */
static void upd_finish(void)
{
    uint8_t digest[32];
    char hex[65];
    upd.state = PANEL_UPD_VERIFY;
    hex[64] = '\0';
    if (mbedtls_sha256_finish(&upd.sha, digest) != 0) {
        upd_fail("sha256");
        return;
    }
    /* 32 digest bytes -> 64 hex chars, two per byte, to compare with the
     * hex string in the manifest. */
    for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (memcmp(hex, upd.manifest.sha256, 64) != 0) {
        upd_fail("image sha256 mismatch");
        return;
    }
    /* esp_ota_end also validates the image format (header, segments,
     * checksum). Only then is the boot partition switched. The new image
     * boots as PENDING_VERIFY; main.c marks it valid once it receives a
     * real stream, otherwise the bootloader rolls back on the next reset. */
    esp_err_t err = esp_ota_end(upd.ota);
    upd.ota = 0;
    upd.ota_open = false;
    if (err != ESP_OK) {
        upd_fail("esp_ota_end rejected the image");
        return;
    }
    if (esp_ota_set_boot_partition(upd.partition) != ESP_OK) {
        upd_fail("could not switch boot partition");
        return;
    }
    upd_reset_socket();
    mbedtls_sha256_free(&upd.sha);
    upd.state = PANEL_UPD_DONE;
    upd.phase = UPD_FINISH;
    ESP_LOGI(TAG, "OTA verified, restart pending");
}

/* Returns a pointer to the "\r\n\r\n" that ends an HTTP header, or NULL if
 * it hasn't arrived yet. */
static char *find_header_end(char *buf, size_t len)
{
    for (size_t i = 0; i + 3 < len; ++i)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return buf + i;
    return NULL;
}

bool panel_upd_start(const char *host_ip, uint16_t port, const panel_upd_t *manifest)
{
    if (!host_ip || !manifest || strlen(host_ip) >= sizeof upd.host
            || manifest->size < UPD_MIN_BYTES || manifest->size > UPD_MAX_BYTES)
        return false;
    if (upd.state == PANEL_UPD_CONNECT || upd.state == PANEL_UPD_DOWNLOAD
            || upd.state == PANEL_UPD_VERIFY)
        return false;
    memset(&upd, 0, sizeof upd);
    upd.sock = -1;
    upd.state = PANEL_UPD_CONNECT;
    upd.phase = UPD_CONNECT;
    snprintf(upd.host, sizeof upd.host, "%s", host_ip);
    upd.port = port;
    upd.manifest = *manifest;
    mbedtls_sha256_init(&upd.sha);
    mbedtls_sha256_starts(&upd.sha, 0);
    upd.deadline_ms = now_ms() + UPD_CONNECT_TIMEOUT_MS;
    return true;
}

void panel_upd_abort(void)
{
    upd_reset_socket();
    if (upd.ota_open) {
        esp_ota_abort(upd.ota);
        upd.ota_open = false;
    }
    upd.ota = 0;
    mbedtls_sha256_free(&upd.sha);
    upd.state = PANEL_UPD_IDLE;
    upd.phase = UPD_FINISH;
}

/* Percent uses 64-bit arithmetic: received * 100 overflows 32 bits above
 * 42 MB. Not possible with a 3 MB image, but it costs nothing. */
panel_upd_state_t panel_upd_poll(int *percent)
{
    if (percent) {
        if (upd.state == PANEL_UPD_DONE) *percent = 100;
        else if (upd.state == PANEL_UPD_IDLE || upd.state == PANEL_UPD_FAIL || !upd.want)
            *percent = -1;
        else *percent = (int)((uint64_t)upd.received * 100 / upd.want);
    }
    if (upd.state == PANEL_UPD_IDLE || upd.phase == UPD_FINISH) return upd.state;
    int64_t now = now_ms();

    switch (upd.phase) {
    case UPD_CONNECT: {
        struct sockaddr_in peer = {.sin_family = AF_INET, .sin_port = htons(upd.port)};
        ip4_addr_t addr;
        if (ip4addr_aton(upd.host, &addr) != 1) {
            upd_fail("invalid host IP");
            return upd.state;
        }
        peer.sin_addr.s_addr = addr.addr;
        if (upd.sock < 0) {
            upd.sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (upd.sock < 0) { upd_fail("no tcp socket"); return upd.state; }
            /* Non-blocking connect: connect() returns EINPROGRESS straight
             * away and the socket becomes writable when the handshake ends.
             * select() with a zero timeout just polls for that. */
            int flags = fcntl(upd.sock, F_GETFL, 0);
            fcntl(upd.sock, F_SETFL, flags | O_NONBLOCK);
            int result = connect(upd.sock, (struct sockaddr *)&peer, sizeof peer);
            if (result != 0 && errno != EINPROGRESS) {
                upd_fail("connection refused");
                return upd.state;
            }
            upd.deadline_ms = now + UPD_CONNECT_TIMEOUT_MS;
        }
        struct timeval zero = {0, 0};
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(upd.sock, &writable);
        int ready = select(upd.sock + 1, NULL, &writable, NULL, &zero);
        if (ready < 0 && errno != EINTR) { upd_fail("select"); return upd.state; }
        if (ready == 1) {
            /* Writable can also mean "failed". SO_ERROR tells which. */
            int so_error = 0;
            socklen_t length = sizeof so_error;
            getsockopt(upd.sock, SOL_SOCKET, SO_ERROR, &so_error, &length);
            if (so_error) { upd_fail("connection error"); return upd.state; }
            upd.sent = 0;
            upd.phase = UPD_REQUEST;
        } else if (now > upd.deadline_ms) {
            upd_fail("connection timed out");
        }
        return upd.state;
    }
    case UPD_REQUEST: {
        char request[192];
        int n = snprintf(request, sizeof request,
            "GET /firmware.bin HTTP/1.0\r\nHost: %s\r\nUser-Agent: panel-ota\r\n"
            "Accept-Encoding: identity\r\nConnection: close\r\n\r\n", upd.host);
        if (n <= 0 || (size_t)n >= sizeof request) { upd_fail("request too long"); return upd.state; }
        ssize_t sent = send(upd.sock, request + upd.sent, n - upd.sent, 0);
        if (sent > 0) {
            upd.sent += (size_t)sent;
            if (upd.sent >= (size_t)n) { upd.phase = UPD_HEADERS; upd.deadline_ms = now + UPD_HEADER_TIMEOUT_MS; }
        } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            upd_fail("request failed");
        } else if (now > upd.deadline_ms) {
            upd_fail("request stalled");
        }
        return upd.state;
    }
    case UPD_HEADERS: {
        ssize_t got = recv(upd.sock, upd.buf + upd.head_len,
                           sizeof upd.buf - upd.head_len, 0);
        if (got > 0) {
            upd.head_len += (size_t)got;
            upd.deadline_ms = now + UPD_HEADER_TIMEOUT_MS;
        } else if (got == 0) {
            upd_fail("connection closed before the header");
            return upd.state;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            upd_fail("header read failed");
            return upd.state;
        } else if (now > upd.deadline_ms) {
            upd_fail("empty header");
            return upd.state;
        }

        char *end = find_header_end((char *)upd.buf, upd.head_len);
        if (!end) {
            if (upd.head_len >= UPD_HEADER_MAX) { upd_fail("header too long"); return upd.state; }
            return upd.state;
        }
        size_t hlen = (size_t)(end - (char *)upd.buf);
        if (hlen > UPD_HEADER_MAX) { upd_fail("header too long"); return upd.state; }

        upd.buf[hlen] = '\0';
        if (strncmp((char *)upd.buf, "HTTP/1.", 7) != 0) {
            upd_fail("not an HTTP/1.x response");
            return upd.state;
        }
        char *space = strchr((char *)upd.buf, ' ');
        unsigned status = space ? (unsigned)strtoul(space + 1, NULL, 10) : 0;
        if (status != 200) { upd_fail("HTTP status is not 200"); return upd.state; }

        /* Walk the header lines after the status line. Each line is
         * temporarily NUL terminated (saved/restored) so the string
         * functions stop at its end. Header names are case insensitive. */
        unsigned long content_length = 0;
        bool have_length = false, chunked = false, encoded = false;
        char *line = (char *)upd.buf;
        char *eol = strstr(line, "\r\n");
        if (!eol) { upd_fail("unreadable http header"); return upd.state; }
        for (line = eol + 2; line < (char *)upd.buf + hlen; line = eol + 2) {
            eol = strstr(line, "\r\n");
            if (!eol || eol > (char *)upd.buf + hlen) eol = (char *)upd.buf + hlen;
            char saved = *eol;
            *eol = '\0';
            if (!strncasecmp(line, "Content-Length:", 15)) {
                char *tail = NULL;
                content_length = strtoul(line + 15, &tail, 10);
                while (tail && (*tail == ' ' || *tail == '\t')) ++tail;
                have_length = tail && *tail == '\0' && content_length >= UPD_MIN_BYTES
                           && content_length <= UPD_MAX_BYTES;
            } else if (!strncasecmp(line, "Transfer-Encoding:", 18)) {
                chunked = true;
            } else if (!strncasecmp(line, "Content-Encoding:", 17)) {
                encoded = true;
            }
            *eol = saved;
            if (eol >= (char *)upd.buf + hlen) break;
        }
        /* We need the exact size in advance: chunked transfer or gzip
         * would make the byte count meaningless for the checks below. */
        if (!have_length || chunked || encoded) {
            upd_fail("bad content length");
            return upd.state;
        }
        if (content_length != upd.manifest.size) {
            upd_fail("Content-Length differs from the manifest");
            return upd.state;
        }
        upd.want = upd.manifest.size;

        /* The same recv() that brought the end of the header may also have
         * brought the first body bytes: they start right after "\r\n\r\n". */
        size_t rest = upd.head_len - (hlen + 4);
        upd.head_len = 0;
        upd.deadline_ms = now + UPD_IDLE_TIMEOUT_MS;
        upd.body_started_ms = now;
        upd.phase = UPD_BODY;
        upd.state = PANEL_UPD_DOWNLOAD;
        if (!upd_body(upd.buf + hlen + 4, rest)) return upd.state;
        if (upd.received == upd.want) upd_finish();
        return upd.state;
    }
    case UPD_BODY: {
        if (upd.received == upd.want) { upd_finish(); return upd.state; }
        if (now > upd.deadline_ms) { upd_fail("download stalled"); return upd.state; }
        /* Minimum average rate. After t ms we expect at least
         *   t * 6000 / 1000 bytes
         * and allow one buffer (8 KiB) of slack for TCP bursts. Falling
         * further behind means a sick link: give up and try later instead
         * of hanging on the update screen. */
        if (upd.received
                && (uint64_t)(now - upd.body_started_ms) * UPD_TOTAL_MIN_RATE / 1000
                    > upd.received + UPD_BUFFER) {
            upd_fail("download too slow");
            return upd.state;
        }
        ssize_t got = 0;
        bool eof = false;
        /* Drain until the buffer is full: flash prefers big chunks and the
         * TCP window opens up in one go. EAGAIN is not the end of the body,
         * the socket is non-blocking and more data may come in the next
         * packet. Only a peer close (recv == 0) or a real error ends it. */
        while (got < (ssize_t)sizeof upd.buf) {
            ssize_t chunk = recv(upd.sock, upd.buf + got,
                                 sizeof upd.buf - (size_t)got, 0);
            if (chunk > 0) {
                got += chunk;
                if (upd.received + (uint32_t)got >= upd.want) break;
            } else if (chunk == 0) {
                eof = true;
                break;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                break;  /* nothing right now, write what we have */
            } else {
                upd_fail("connection lost during download");
                return upd.state;
            }
        }
        if (got > 0) {
            upd.deadline_ms = now + UPD_IDLE_TIMEOUT_MS;
            if (upd_body(upd.buf, (size_t)got) && upd.received == upd.want) upd_finish();
        } else if (eof) {
            upd_fail(upd.received < upd.want ? "download truncated" : "unexpected end of stream");
        }
        return upd.state;
    }
    default:
        return upd.state;
    }
}
