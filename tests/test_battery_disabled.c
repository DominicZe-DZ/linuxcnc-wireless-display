/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * Checks the real battery API builds and works with the feature disabled. */
#include "battery.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    battery_status_t status = {.valid=true, .charging=true, .percent=50};
    battery_init();
    assert(!battery_poll(1000, &status));
    assert(!status.valid && !status.charging && status.percent == 0);
    assert(!battery_poll(600000, &status));
    assert(!status.valid);
    puts("battery disabled: API works without ADC or dependent config");
    return 0;
}
