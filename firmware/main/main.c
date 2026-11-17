/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * main.c - firmware entry point and main loop.
 *
 * Everything the application does happens in ONE loop in app_main(), every
 * 5 ms. Networking is non-blocking and shares the loop with LVGL, so no other
 * task ever touches a graphics object and no locks are needed. (Wi-Fi and
 * the LCD DMA do run in their own ESP-IDF tasks / interrupts, but they only
 * talk to us through events and the counters below.)
 *
 * One pass of the loop:
 *
 *   1. Wi-Fi state: if it changed, drop the socket and start a new session.
 *   2. Staleness: no valid sample for PANEL_STALE_MS -> new nonce, rediscover.
 *   3. Pending command older than 2.5 s -> report "timeout" to the page.
 *   4. (Re)open the UDP socket if needed (at most every 2 s).
 *   5. Send a hello once per second (broadcast until the PC is known).
 *   6. Read up to 8 datagrams: ACKs for our command, or telemetry samples.
 *   7. OTA: start a download if a newer, correctly signed image is offered.
 *   8. Battery: read the ADC once per second.
 *   9. Every 40 ms: push the latest sample and battery to the screen.
 *  10. OTA progress / restart / failure handling.
 *  11. lv_timer_handler(): LVGL renders and reads the touch panel.
 *  12. If the user queued a command, sign it and send it.
 *
 * Session identifiers (all random, 64 bit, written as 16 hex chars):
 *   nonce    made by THIS display, renewed every time the link is lost. A
 *            sample must echo it, so packets from an older session can never
 *            be accepted again.
 *   session  made by the PC for each run of panel_wifi.
 *   id       made by this display for each command.
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "sdkconfig.h"
#include "I2C_Driver.h"
#include "TCA9554PWR.h"
#include "ST7701S.h"
#include "GT911.h"
#include "LVGL_Driver.h"
#include "dashboard.h"
#include "battery.h"
#include "pages.h"
#include "control.h"
#include "update.h"
#include "esp_system.h"
#include "version.h"
#include "esp_ota_ops.h"

static esp_netif_t *wifi_netif;
/* Incremented by the Wi-Fi event handler (which runs in the IDF event task)
 * on every start, disconnect and new IP. The main loop compares it with its
 * own copy: any difference means "the link changed, start over". A counter
 * instead of a flag avoids a lost event when two happen between two loop
 * passes; volatile because it is written from another task. A single 32-bit
 * aligned store/load is atomic on the ESP32, so no lock is needed. */
static volatile uint32_t wifi_generation;
static bool upd_started;        /* an OTA download is running */
static bool boot_confirmed;     /* this image was already marked valid */
static int64_t upd_retry_at;    /* ms: no OTA attempt before this time */

/* Marks the running image as good. Called once, after the first valid
 * telemetry sample: at that point Wi-Fi, the protocol and the screen have
 * all been proven to work with this build, so rollback is cancelled. */
static void confirm_boot_once(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if (state == ESP_OTA_IMG_VALID) return;
    /* PENDING_VERIFY after an OTA, UNDEFINED after flashing over USB with an
     * erased otadata. Either way the running image works, so mark it valid;
     * the update logic below only looks at offers from a VALID image. */
    esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI("panel", "image confirmed (state %d): %s", (int)state,
             esp_err_to_name(result));
}

/* Private settings (Wi-Fi, machine ID, control key) live in NVS so they can be
 * flashed over USB without rebuilding and never end up in Git. The menuconfig
 * values are only a fallback. */
static void setting(const char *key, const char *fallback, char *out, size_t capacity)
{
    snprintf(out, capacity, "%s", fallback);
    nvs_handle_t handle;
    if (nvs_open("panel", NVS_READONLY, &handle) != ESP_OK) return;
    size_t size = capacity;
    if (nvs_get_str(handle, key, out, &size) != ESP_OK)
        snprintf(out, capacity, "%s", fallback);
    nvs_close(handle);
}

/* Wi-Fi / IP event handler (runs in the IDF event task, NOT in the main
 * loop). It only reconnects and bumps wifi_generation. */
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        ++wifi_generation;
        esp_wifi_connect(); /* asynchronous, does not wait for the AP */
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) ++wifi_generation;
}

/* Station mode with the SSID/password from NVS (or menuconfig). The
 * credentials are kept in RAM only (WIFI_STORAGE_RAM) so the Wi-Fi driver
 * doesn't keep its own second copy in flash. Power save is off: modem sleep
 * adds up to ~100 ms of latency and makes 25 Hz telemetry jittery, and the
 * display is mains/USB powered most of the time anyway. */
static void wifi_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    wifi_config_t cfg = {0};
    setting("ssid", CONFIG_PANEL_WIFI_SSID, (char *)cfg.sta.ssid, sizeof cfg.sta.ssid);
    setting("password", CONFIG_PANEL_WIFI_PASSWORD, (char *)cfg.sta.password, sizeof cfg.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

/* 64 random bits from the hardware RNG (esp_random() is a true RNG while
 * the radio is on) written as 16 hex chars: two 32-bit values, each as 8
 * zero-padded hex digits. */
static void new_nonce(char nonce[17])
{
    snprintf(nonce, 17, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
}

void app_main(void)
{
    I2C_Init();
    ESP_ERROR_CHECK(EXIO_Init());
    LCD_Init();
    /* A lower pixel clock leaves PSRAM bandwidth for Wi-Fi and LVGL.
     * The driver applies the new clock on the next VSYNC. */
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_set_pclk(panel_handle, CONFIG_PANEL_LCD_PCLK_MHZ * 1000000));
    Touch_Init();
    LVGL_Init();
    dashboard_init();
    battery_init();
    wifi_init();

    /* Address of the PC. The port is fixed; the IP is learned from the first
     * valid sample (host_known). htons(): network byte order (big endian),
     * the ESP32 is little endian. */
    struct sockaddr_in host = {.sin_family = AF_INET, .sin_port = htons(CONFIG_PANEL_HOST_PORT)};
    char machine[33], screen[13];
    char control_key[65];
    setting("control_key","",control_key,sizeof control_key);
    pages_control_ready(panel_key_valid(control_key));
    setting("machine_id", CONFIG_PANEL_MACHINE_ID, machine, sizeof machine);
    if (strlen(machine) < 1 || strspn(machine, "abcdefghijklmnopqrstuvwxyz0123456789_-") != strlen(machine))
        ESP_ERROR_CHECK(ESP_ERR_INVALID_ARG);
    /* Screen ID = Wi-Fi MAC as 12 lower case hex digits, no separators,
     * e.g. a1:b2:c3:d4:e5:f6 -> "a1b2c3d4e5f6". Unique per board and printed
     * on the log so it can be copied into host.json. */
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(screen, sizeof screen, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI("panel", "discovery v2: machine=%s screen=%s", machine, screen);
    int sock = -1;
    panel_sample_t current = {0};
    bool have_sample = false, host_known = false;
    uint32_t local_ip = 0, generation = wifi_generation;
    char nonce[17];
    new_nonce(nonce);
    /* Timestamps in ms since boot. hello_ms / draw_ms start in the past so
     * the first hello and the first draw happen immediately. */
    int64_t received_ms = 0, hello_ms = -1000, draw_ms = -40, socket_retry_ms = 0;
    /* The one command in flight: its random id and the session it was sent
     * in. An empty pending_id means "no command waiting for an ACK". */
    char pending_id[17]="", pending_session[17]="";
    int64_t pending_since=0;
    while (true) {
        int64_t now = esp_timer_get_time() / 1000;
        wifi_ap_record_t ap;
        esp_netif_ip_info_t ip = {0};
        bool wifi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK
            && esp_netif_get_ip_info(wifi_netif, &ip) == ESP_OK && ip.ip.addr != 0;
        /* 1. Link changed (event counter moved or our IP changed): close the
         *    socket and forget the PC; a new nonce starts a new session. */
        if (generation != wifi_generation || (wifi ? ip.ip.addr : 0) != local_ip) {
            generation = wifi_generation;
            local_ip = wifi ? ip.ip.addr : 0;
            if (sock >= 0) close(sock);
            sock = -1;
            have_sample = host_known = false;
            new_nonce(nonce);
            hello_ms = -1000;
            socket_retry_ms = now;
        }
        /* 2. Staleness. */
        bool stale = have_sample && (now - received_ms > CONFIG_PANEL_STALE_MS || !wifi);
        if (stale) {
            /* A fresh nonce discards every packet from the previous session,
             * even if they show up late after a host restart or Wi-Fi drop. */
            have_sample = false;
            host_known = false;
            ESP_LOGW("panel", "data stale, asking for a new stream");
            new_nonce(nonce);
            hello_ms = -1000;
        }
        /* 3. A command gets 2.5 s for its ACK. After that it is reported as
         *    unconfirmed; it is NOT resent (it may already have run). */
        if (pending_id[0] && (!have_sample || !wifi || now-pending_since>2500)) {
            pending_id[0]=0;pages_result("timeout");
        }
        /* 4. UDP socket on the display port, any local address, broadcast
         *    allowed (needed for the hello), non-blocking. */
        if (wifi && sock < 0 && now >= socket_retry_ms) {
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            int allow_broadcast = 1;
            struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(CONFIG_PANEL_SCREEN_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
            if (sock >= 0 && (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &allow_broadcast, sizeof allow_broadcast) != 0
                    || bind(sock, (struct sockaddr *)&local, sizeof local) != 0 || fcntl(sock, F_SETFL, O_NONBLOCK) < 0)) {
                close(sock); sock = -1;
            }
            socket_retry_ms = now + 2000;
        }
        if (wifi && sock >= 0) {
            if (now - hello_ms >= 1000) {
                char hello[190];
                int length = snprintf(hello, sizeof hello, "{\"v\":2,\"type\":\"hello\",\"machine\":\"%s\",\"screen\":\"%s\",\"nonce\":\"%s\"}", machine, screen, nonce);
                /* 5. Hello. Until the PC is known it goes to the subnet's
                 * directed broadcast address: our IP with all host bits set.
                 *   IP 192.168.1.37, mask 255.255.255.0
                 *   ~mask = 0.0.0.255 -> IP | ~mask = 192.168.1.255
                 * Both values are in network byte order; bitwise operations
                 * don't care about byte order, so no conversion is needed. */
                struct sockaddr_in target = host;
                if (!host_known) target.sin_addr.s_addr = ip.ip.addr | ~ip.netmask.addr;
                sendto(sock, hello, length, 0, (struct sockaddr *)&target, sizeof target);
                hello_ms = now;
            }
            /* 6. At most 8 packets per loop so the network can't starve LVGL.
             * Only the highest sequence number among them is kept. */
            for (int i = 0; i < 8; ++i) {
                /* Only this task uses the buffer. Keeping it off the stack
                 * leaves room for LVGL and printf with floats. */
                static char packet[1202];
                struct sockaddr_in peer;
                socklen_t peer_size = sizeof peer;
                int length = recvfrom(sock, packet, sizeof packet - 1, 0, (struct sockaddr *)&peer, &peer_size);
                if (length < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) { close(sock); sock = -1; }
                    break;
                }
                /* Source filter: right port, same subnet as us (network part of
                 * both addresses equal: addr & mask), and once the PC is known
                 * exactly that PC. Anything else is ignored before parsing. */
                if (length > 1200 || peer.sin_port != host.sin_port
                        || (peer.sin_addr.s_addr & ip.netmask.addr) != (ip.ip.addr & ip.netmask.addr)
                        || (host_known && peer.sin_addr.s_addr != host.sin_addr.s_addr)) continue;
                packet[length] = '\0';
                if (pending_id[0] && host_known) {
                    char result[24];
                    if (panel_ack_decode(packet,length,machine,screen,pending_session,nonce,pending_id,control_key,result)) {
                        pages_result(result);
                        if (strcmp(result,"pending")) pending_id[0]=0;
                        continue;
                    }
                }
                /* Telemetry: must be valid for our nonce, and inside the
                 * current session newer than what we have (drops duplicates
                 * and packets reordered by the network). */
                panel_sample_t next;
                if (!panel_decode(packet, length, nonce, machine, screen, &next)) continue;
                if (have_sample && (strcmp(current.session, next.session) != 0 || !panel_sequence_newer(next.seq, current.seq))) continue;
                if (!have_sample) {
                    host = peer;
                    host_known = true;
                    ESP_LOGI("panel", "stream accepted: %s, seq=%lu, RPM=%.0f", next.demo ? "DEMO" : "HAL", (unsigned long)next.seq, (double)next.rpm);
                }
                current = next;
                received_ms = now;
                have_sample = true;
                if (!boot_confirmed) { boot_confirmed = true; confirm_boot_once(); }
            }
        }
        /* 7. OTA. Only when: this image is confirmed good, nothing else is
         *    running, the PC offers a DIFFERENT build ID, the machine is in
         *    idle manual mode (never during a job) and no command is pending.
         *    The offer must also carry our current nonce. */
        if (boot_confirmed && !upd_started && have_sample && current.upd_present && wifi
                && now > upd_retry_at && strcmp(current.upd_version, PANEL_BUILD_ID) != 0
                && !strcmp(current.state, "manual") && !pending_id[0]
                && !strcmp(current.nonce, nonce)) {
            esp_ota_img_states_t img_state;
            const esp_partition_t *running = esp_ota_get_running_partition();
            if (running && esp_ota_get_state_partition(running, &img_state) == ESP_OK
                    && img_state == ESP_OTA_IMG_VALID) {
                panel_upd_t offer;
                /* The signature covers the current nonce, so a captured
                 * manifest can't be replayed in another session. */
                char fw_json[320];
                snprintf(fw_json, sizeof fw_json,
                    "{\"version\":\"%s\",\"size\":%lu,\"sha256\":\"%s\",\"mac\":\"%s\"}",
                    current.upd_version, (unsigned long)current.upd_size,
                    current.upd_sha, current.upd_mac);
                if (panel_upd_parse(fw_json, strlen(fw_json), control_key,
                        current.nonce, &offer)
                        && strcmp(offer.version, PANEL_BUILD_ID) != 0
                        && panel_upd_start(inet_ntoa(host.sin_addr), CONFIG_PANEL_UPDATE_PORT, &offer)) {
                    upd_started = true;
                    pages_control_ready(false);
                    pending_id[0] = 0;
                    ESP_LOGW("panel", "update started: %s", offer.version);
                } else {
                    upd_retry_at = now + 600000;  /* try again in 10 min */
                }
            }
        }
        /* 8-9. Battery (1 Hz inside battery_poll) and screen update at 25 Hz.
         * Drawing is decoupled from packet arrival: several packets between
         * two draws only cost one update, with the newest values. */
        battery_status_t battery;
        battery_poll(now, &battery);
        if (now - draw_ms >= 40) {
            dashboard_update(&current, have_sample, wifi);
            dashboard_battery(&battery);
            draw_ms = now;
        }
        /* 10. OTA progress. On success give LVGL one pass to show "verified"
         * and restart into the new image. */
        if (upd_started) {
            int percent = -1;
            panel_upd_state_t state = panel_upd_poll(&percent);
            pages_ota(state, percent);
            if (state == PANEL_UPD_DONE) {
                lv_timer_handler();
                vTaskDelay(pdMS_TO_TICKS(600));
                esp_restart();
            }
            if (state == PANEL_UPD_FAIL) {
                upd_started = false;
                upd_retry_at = now + 600000;
                pages_ota_end();
                pages_control_ready(panel_key_valid(control_key));
                ESP_LOGW("panel", "update failed, running image untouched");
            }
        }
        /* 11. LVGL: runs timers, reads touch, renders dirty areas. */
        lv_timer_handler();
        /* 12. Command queued by pages.c. Dropped locally (and reported as
         * such) unless there is a fresh, non-demo stream, a known PC and no
         * other command in flight. */
        panel_command_t command;
        if (pages_take_command(&command)) {
            /* Commands are also sent while the host reports "mode" or
             * "remote" (MDI left over by the host itself): its worker
             * checks everything again and can switch back before running.
             * Stop is always attempted while a stream is alive; the host
             * re-validates it and replies with the reason if refused. */
            bool stop_request=!strcmp(command.action,"stop");
            if (upd_started || !have_sample || !wifi || sock<0 || !host_known || current.demo || pending_id[0]
                    || (!stop_request && !current.control_allowed && strcmp(current.state,"mode")
                        && strcmp(current.state,"remote"))) pages_result("dropped");
            else {
                new_nonce(pending_id);strcpy(pending_session,current.session);
                char request[600];
                int length=panel_command_encode(request,sizeof request,&command,&current,machine,screen,nonce,pending_id,control_key);
                if (length && sendto(sock,request,length,0,(struct sockaddr*)&host,sizeof host)==length) pending_since=now;
                else {pending_id[0]=0;pages_result("error");}
            }
        }
        /* Yield 5 ms so the idle task and Wi-Fi get CPU time. The loop thus
         * runs at up to ~200 Hz, far faster than the 25 Hz telemetry. */
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
