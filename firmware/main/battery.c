/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * battery.c - reads the LiPo voltage on ADC1 once per second from the main
 * loop and feeds it to the battery model (battery_model.c).
 *
 * Signal chain on the 2.8C board:
 *
 *   cell (+) --[R5 200k]--+--[R9 100k]-- GND
 *                         |
 *                     BAT_ADC (GPIO4)        V_adc = V_cell * 100/(200+100)
 *                                                  = V_cell / 3
 *
 * A full cell (4.2 V) gives 1.4 V at the pin, well inside the range of the
 * 12 dB attenuation setting (about 0..3.1 V on the ESP32-S3).
 *
 * ADC1 is used because ADC2 is shared with the Wi-Fi radio and can't be
 * read reliably while Wi-Fi is on.
 */
#include "battery.h"
#include "sdkconfig.h"
#if CONFIG_PANEL_BATTERY
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static adc_oneshot_unit_handle_t unit;
static adc_cali_handle_t cali;
static adc_channel_t channel;
static bool ready;
static int64_t last_ms = -1000000;   /* far in the past: first poll reads at once */
#endif
static battery_model_t model;

void battery_init(void)
{
    battery_model_init(&model);
#if CONFIG_PANEL_BATTERY
    adc_unit_t adc_unit;
    if (adc_oneshot_io_to_channel(CONFIG_PANEL_BATTERY_GPIO, &adc_unit, &channel) != ESP_OK || adc_unit != ADC_UNIT_1) {
        ESP_LOGE("battery", "GPIO%d is not an ADC1 channel", CONFIG_PANEL_BATTERY_GPIO);
        return;
    }
    adc_oneshot_unit_init_cfg_t init = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&init, &unit) != ESP_OK) return;
    adc_oneshot_chan_cfg_t cfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_oneshot_config_channel(unit, channel, &cfg) != ESP_OK) return;
    /* Factory calibration from eFuse. Converts raw counts to millivolts
     * with a fitted curve, which is much more accurate than raw * 3100/4095
     * because the ESP32 ADC is not linear near the ends of its range. */
    adc_cali_curve_fitting_config_t cal = {.unit_id = ADC_UNIT_1, .chan = channel, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_cali_create_scheme_curve_fitting(&cal, &cali) != ESP_OK) {
        ESP_LOGE("battery", "no ADC calibration in eFuse, hiding the percentage");
        return;
    }
    ready = true;
    ESP_LOGI("battery", "battery on GPIO%d, divider x%.3f", CONFIG_PANEL_BATTERY_GPIO, CONFIG_PANEL_BATTERY_DIVIDER_X1000 / 1000.0);
#endif
}

bool battery_poll(int64_t now_ms, battery_status_t *out)
{
    bool changed = false;
#if CONFIG_PANEL_BATTERY
    if (ready && now_ms - last_ms >= 1000) {
        last_ms = now_ms;
        int total = 0, count = 0;
        for (int i = 0; i < 8; ++i) {
            int raw, mv;
            if (adc_oneshot_read(unit, channel, &raw) == ESP_OK && adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK) {
                total += mv;
                ++count;
            }
        }
        /* Average of the good readings, in mV at the pin. Back to the cell:
         *   V_cell [V] = mV_pin * (DIVIDER_X1000 / 1000) / 1000
         *              = mV_pin * DIVIDER_X1000 / 1e6
         * e.g. 1400 mV * 3000 / 1e6 = 4.2 V. If no reading worked, 0 V,
         * which the model treats as "no cell / no reading". */
        float volts = count ? total / (float)count * CONFIG_PANEL_BATTERY_DIVIDER_X1000 / 1.0e6f : 0.0f;
        changed = battery_model_feed(&model, now_ms, volts);
        if (changed)
            ESP_LOGI("battery", "%.2f V, %d %%, %s", (double)model.status.volts, model.status.percent,
                     model.status.charging ? "charging" : "not charging");
    }
#else
    /* With PANEL_BATTERY=n Kconfig doesn't define the GPIO/divider symbols,
     * so don't reference them or claim the ADC at all. */
    (void)now_ms;
#endif
    *out = model.status;
    return changed;
}
