/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * Tests the same C parser that runs on the ESP32, no display needed. */
#include "telemetry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool decode(const char *json, size_t length, const char *nonce, panel_sample_t *out)
{
    return panel_decode(json, length, nonce, "emco-f1", "001122334455", out);
}

int main(void)
{
    const char *good = "{\"v\":2,\"machine\":\"emco-f1\",\"screen\":\"001122334455\",\"type\":\"sample\",\"session\":\"aaaaaaaaaaaaaaaa\",\"nonce\":\"bbbbbbbbbbbbbbbb\",\"seq\":4294967295,\"valid\":true,\"demo\":false,\"data\":{\"rpm\":2450,\"feed\":10,\"x\":null,\"spindle_on\":true}}";
    panel_sample_t sample = {0};
    assert(decode(good, strlen(good), "bbbbbbbbbbbbbbbb", &sample));
    assert(!panel_decode(good, strlen(good), "bbbbbbbbbbbbbbbb", "other-machine", "001122334455", &sample));
    assert(!panel_decode(good, strlen(good), "bbbbbbbbbbbbbbbb", "emco-f1", "ffffffffffff", &sample));
    assert(sample.rpm == 2450 && sample.feed == 10 && sample.present == 35);
    assert(sample.valid && !sample.demo && sample.seq == UINT32_MAX);
    assert(!decode(good, strlen(good), "cccccccccccccccc", &sample));
    assert(!decode("[]", 2, "bbbbbbbbbbbbbbbb", &sample));
    char packet[1400];
    snprintf(packet, sizeof packet, "%s garbage", good);
    assert(!decode(packet, strlen(packet), "bbbbbbbbbbbbbbbb", &sample));
    for (size_t i = 0; i < strlen(good); ++i)
        assert(!decode(good, i, "bbbbbbbbbbbbbbbb", &sample));
    assert(sample.rpm == 2450); /* errors leave the sample untouched */
    assert(panel_sequence_newer(0, UINT32_MAX));
    assert(!panel_sequence_newer(2, 3));
    assert(!panel_sequence_newer(3, 3));
    assert(!sample.ui_available && !sample.control_allowed);
    const char *ui="{\"state\":\"manual\",\"allowed\":true,\"token\":\"1234567890abcdef\",\"wcs\":2,\"selected\":5000,\"work\":[1.25,-2,0]}";
    snprintf(packet,sizeof packet,"%.*s,\"ui\":%s}",(int)strlen(good)-1,good,ui);
    assert(decode(packet,strlen(packet),"bbbbbbbbbbbbbbbb",&sample));
    assert(sample.ui_available&&sample.control_allowed&&sample.work_present&&sample.wcs==2);
    assert(sample.work[0]==1.25f&&sample.selected_rpm==5000);
    char *bad=strstr(packet,"5000");memcpy(bad,"5001",4);
    assert(!decode(packet,strlen(packet),"bbbbbbbbbbbbbbbb",&sample));
    assert(sample.selected_rpm==5000); /* the ui extension is all-or-nothing too */
    /* OTA offer (ui.upd): a valid one is parsed, a malformed one does NOT
     * invalidate the sample (optional metadata can't break telemetry). */
    const char *sha="00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    const char *mac="ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100";
    char offer[420];
    snprintf(offer,sizeof offer,
        "{\"state\":\"manual\",\"allowed\":true,\"token\":\"1234567890abcdef\","
        "\"wcs\":2,\"selected\":1500,\"work\":[1,2,3],"
        "\"upd\":{\"version\":\"3ff769c\",\"size\":1278752,"
        "\"sha256\":\"%s\",\"mac\":\"%s\"}}",sha,mac);
    snprintf(packet,sizeof packet,"%.*s,\"ui\":%s}",(int)strlen(good)-1,good,offer);
    assert(decode(packet,strlen(packet),"bbbbbbbbbbbbbbbb",&sample));
    assert(sample.upd_present&&!strcmp(sample.upd_version,"3ff769c")&&sample.upd_size==1278752);
    /* 16 char version: offer ignored, sample still valid. */
    snprintf(offer,sizeof offer,
        "{\"state\":\"manual\",\"allowed\":true,\"token\":\"1234567890abcdef\","
        "\"wcs\":2,\"selected\":1500,\"work\":[1,2,3],"
        "\"upd\":{\"version\":\"0123456789abcdef\",\"size\":1278752,"
        "\"sha256\":\"%s\",\"mac\":\"%s\"}}",sha,mac);
    snprintf(packet,sizeof packet,"%.*s,\"ui\":%s}",(int)strlen(good)-1,good,offer);
    assert(decode(packet,strlen(packet),"bbbbbbbbbbbbbbbb",&sample));
    assert(!sample.upd_present&&sample.ui_available&&sample.control_allowed);
    /* allowed=true while a program runs still grants no control. */
    snprintf(offer,sizeof offer,
        "{\"state\":\"program\",\"allowed\":true,\"token\":\"1234567890abcdef\","
        "\"wcs\":2,\"selected\":1500,\"work\":[1,2,3]}");
    snprintf(packet,sizeof packet,"%.*s,\"ui\":%s}",(int)strlen(good)-1,good,offer);
    assert(decode(packet,strlen(packet),"bbbbbbbbbbbbbbbb",&sample));
    assert(sample.ui_available&&!sample.control_allowed);
    puts("C parser: contract, truncation, nonce and sequence checks OK");
    return 0;
}
