/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * Synthetic vectors shared with Python's hashlib/hmac. The repeated 01 key
 * is a test value only, never a real pairing key. */
#include "control.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    const char *key="0101010101010101010101010101010101010101010101010101010101010101";
    panel_command_t command={.action="speed",.axis="-",.rpm=2500,.wcs=2};
    panel_sample_t sample={0};
    strcpy(sample.session,"1234567890abcdef");strcpy(sample.token,"2222222222222222");
    char packet[600],result[24]="unchanged";
    assert(panel_command_encode(packet,sizeof packet,&command,&sample,"emco-f1","001122334455",
        "fedcba0987654321","1111111111111111",key)>0);
    assert(strstr(packet,"82e8be5ad604532e769a141af576ccb3b0c4c47c468197c699e9927da5ade4f5"));
    const char *ack="{\"v\":3,\"type\":\"ack\",\"machine\":\"emco-f1\",\"screen\":\"001122334455\",\"session\":\"1234567890abcdef\",\"nonce\":\"fedcba0987654321\",\"id\":\"1111111111111111\",\"result\":\"ok\",\"mac\":\"5ec24e712106207fb1f920a9f04b10003b39bb12aed9b3fbd7409964eaf13190\"}";
    assert(panel_ack_decode(ack,strlen(ack),"emco-f1","001122334455","1234567890abcdef","fedcba0987654321","1111111111111111",key,result));
    assert(!strcmp(result,"ok"));
    assert(!panel_ack_decode(ack,strlen(ack),"emco-f1","001122334455","1234567890abcdef","fedcba0987654321","2222222222222222",key,result));
    char altered[600];snprintf(altered,sizeof altered,"%s",ack);
    char *mac=strstr(altered,"5ec24e");assert(mac);*mac='0';
    assert(!panel_ack_decode(altered,strlen(altered),"emco-f1","001122334455","1234567890abcdef","fedcba0987654321","1111111111111111",key,result));
    assert(panel_command_encode(packet,4,&command,&sample,"emco-f1","001122334455","fedcba0987654321","1111111111111111",key)==0);
    puts("control (C): HMAC matches Python, ACK authenticated, wrong ID and tampering rejected");
    return 0;
}
