/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * display_port.c - glue between LVGL and the board's RGB panel and touch.
 * Replaces the LVGL port of the Waveshare demo.
 *
 * How a frame reaches the glass:
 *
 *   LVGL objects --render dirty areas--> draw buffer (480 x 20 px, internal
 *   SRAM) --flush_cb / esp_lcd_panel_draw_bitmap--> framebuffer (480 x 480
 *   RGB565 = 450 KiB, PSRAM) --RGB peripheral + DMA, continuously--> ST7701S
 *
 * The RGB panel has no memory of its own: the ESP32 must stream the whole
 * framebuffer to it ~29 times per second forever. That's why there is one
 * permanent framebuffer that we only patch, never a second full buffer to
 * swap (it would double PSRAM traffic and compete with Wi-Fi).
 *
 * With full_refresh = false LVGL only renders the rectangles that were
 * invalidated, 20 lines at a time, so a needle move costs a few KiB of
 * copying instead of 450 KiB.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <assert.h>
#include "LVGL_Driver.h"
#include "esp_heap_caps.h"

lv_disp_draw_buf_t disp_buf;
lv_disp_drv_t disp_drv;
lv_disp_t *disp;
static lv_color_t *draw_pixels;
static esp_timer_handle_t tick_timer;

/* LVGL calls this for each rendered chunk (area is inclusive: x1..x2,
 * y1..y2). esp_lcd_panel_draw_bitmap() takes an exclusive end, hence +1.
 * It copies the chunk into the framebuffer at that position.
 *
 * Tearing: if we write the framebuffer while the DMA is halfway through
 * scanning it out, the top of the panel shows the old image and the bottom
 * the new one for one frame. So before the FIRST chunk of each refresh we
 * hand over a semaphore and wait until the driver signals the end of a
 * frame (VSYNC). Later chunks of the same refresh go straight through;
 * lv_disp_flush_is_last() tells us when the refresh is complete. */
void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixels)
{
    static bool first_rectangle = true;
#if CONFIG_EXAMPLE_AVOID_TEAR_EFFECT_WITH_SEM
    /* Wait for VSYNC once per refresh, not once per 20 line chunk.
     * Waiting on every chunk makes a full screen change take seconds. */
    if (first_rectangle) {
        xSemaphoreGive(sem_gui_ready);
        xSemaphoreTake(sem_vsync_end, portMAX_DELAY);
    }
#endif
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(drv->user_data, area->x1, area->y1,
                                            area->x2 + 1, area->y2 + 1, pixels));
    first_rectangle = lv_disp_flush_is_last(drv);
    lv_disp_flush_ready(drv);
}

/* esp_timer callback every EXAMPLE_LVGL_TICK_PERIOD_MS: LVGL's time base
 * for animations, timers and touch timing. */
void example_increase_lvgl_tick(void *arg)
{
    (void)arg;
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

/* LVGL polls this to read the GT911 touch controller over I2C. Only the
 * first touch point is used. When released, LVGL keeps the last point. */
void example_touchpad_read(lv_indev_drv_t *driver, lv_indev_data_t *data)
{
    uint16_t x = 0, y = 0;
    uint8_t count = 0;
    esp_lcd_touch_read_data(driver->user_data);
    bool pressed = esp_lcd_touch_get_coordinates(driver->user_data, &x, &y, NULL, &count, 1);
    data->state = pressed && count ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
    if (pressed && count) {
        data->point.x = x;
        data->point.y = y;
    }
}

void LVGL_Init(void)
{
    lv_init();
    /* Draw buffer: 480 px x 20 lines x 2 bytes (RGB565) = 19200 bytes in
     * internal SRAM, which LVGL renders into much faster than PSRAM. */
    const size_t pixels = EXAMPLE_LCD_H_RES * 20;
    draw_pixels = heap_caps_malloc(pixels * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(draw_pixels);
    lv_disp_draw_buf_init(&disp_buf, draw_pixels, NULL, pixels);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = EXAMPLE_LCD_H_RES;
    disp_drv.ver_res = EXAMPLE_LCD_V_RES;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.flush_cb = example_lvgl_flush_cb;
    disp_drv.user_data = panel_handle;
    disp_drv.full_refresh = false;
    disp_drv.direct_mode = false;
    disp = lv_disp_drv_register(&disp_drv);
    static lv_indev_drv_t input;
    lv_indev_drv_init(&input);
    input.type = LV_INDEV_TYPE_POINTER;
    input.disp = disp;
    input.read_cb = example_touchpad_read;
    input.user_data = tp;
    lv_indev_drv_register(&input);
    const esp_timer_create_args_t timer = {.callback = example_increase_lvgl_tick, .name = "lvgl_tick"};
    ESP_ERROR_CHECK(esp_timer_create(&timer, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));
    ESP_LOGI("panel", "LVGL partial refresh: fixed framebuffer, 20 line draw buffer");
}
