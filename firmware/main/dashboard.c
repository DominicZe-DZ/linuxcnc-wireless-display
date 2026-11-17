/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * dashboard.c - the "Classic" page: analog RPM gauge, feed, XYZ, spindle and
 * coolant indicators and the battery icon.
 *
 * Screen layout (480 x 480 px, origin top-left, centre at 240,240):
 *
 *            1000   2000   3000   4000           scale labels (6)
 *        0 /     [DominicZe logo]      \ 5000
 *           |        status text        |        y = 200
 *     EMCO  |   needle pivot (240,240)  |  F1-CNC  y ~ 240
 *           |        850 RPM            |        y = 312 (baseline)
 *           |      F 650 mm/min         |        y = 348
 *            [ X ]   [ Y ]   [ Z ]               y = 380, cards 88 x 28
 *               spindle  coolant                 y = 426, icons 32 x 32
 *                  [battery]                     y = 458
 *
 * Rules that keep the redraw cost low (see docs/design/04-firmware.md):
 *  - Everything runs on the LVGL thread (the main loop in main.c).
 *  - Every value remembers what it last showed (the static old_* variables)
 *    and LVGL is only touched when the visible result changes. LVGL then
 *    redraws just the invalidated rectangles.
 *  - The needle is its own small image on top of everything. Rotating it
 *    invalidates its old and new bounding boxes, not the whole dial.
 */
#include "dashboard.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "dashboard_assets.h"
#include "pages.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
LV_FONT_DECLARE(panel_rpm_48);
LV_FONT_DECLARE(panel_text_21);
LV_FONT_DECLARE(panel_unit_22);

static lv_obj_t *meter, *rpm_label, *rpm_unit, *feed_label, *status_label;
static lv_obj_t *brand, *pointer, *hub;
static lv_obj_t *scale_labels[6], *axis_cards[3], *axis_values[3], *icons[2];
#if CONFIG_PANEL_BATTERY
#include "battery_assets.h"
static lv_obj_t *battery_body, *battery_tip, *battery_fill, *bolt_outline, *bolt;
#endif
/* Colour themes. Index 0 (dark "classic") is the one in use; 2 ("paper",
 * dark ink on light background) is used for the battery icon when the
 * Spindle page shows its light lock screen. Colours are 0xRRGGBB, converted
 * to the panel's RGB565 by lv_color_hex(). */
static int theme;
static lv_obj_t *classic_page;
static const uint32_t backgrounds[] = {0x101519, 0x07181b, 0xe8e3d8};
static const uint32_t foregrounds[] = {0xf3f0e8, 0xecffff, 0x202b2b};
static const uint32_t axis_colors[] = {0x8cdef2, 0xa1e3b4, 0xffd08a};          /* X, Y, Z */
static const uint32_t paper_axis_colors[] = {0x087a9d, 0x247743, 0x946008};

/* Sets a label's text only if it differs from what it shows now. Returns
 * true when it changed. lv_label_set_text() always invalidates the label,
 * even with identical text, so this check is what makes 25 identical
 * samples per second cost zero redraws. */
static bool text_if_changed(lv_obj_t *obj, const char *text)
{
    if (strcmp(lv_label_get_text(obj), text) == 0) return false;
    lv_label_set_text(obj, text);
    return true;
}

/* Creates a label centred horizontally on center_x with its text BASELINE at
 * y = baseline. LVGL positions labels by their top edge, so the top is
 * baseline - line_height + base_line (base_line = pixels below the baseline
 * for descenders). Placing by baseline keeps text of different fonts lined
 * up the way a designer would draw it. */
static lv_obj_t *label(const char *text, int center_x, int baseline, const lv_font_t *font)
{
    lv_obj_t *obj = lv_label_create(classic_page);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_label_set_text(obj, text);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, center_x - 240,
                 baseline - font->line_height + font->base_line);
    return obj;
}

/* Places an ALPHA_8BIT mask (logo or icon) and paints it with one colour.
 * The mask only holds coverage (0 = transparent, 255 = solid); recolor with
 * full opacity gives it its colour, so one mask can be green, red or grey
 * without storing three images. */
static lv_obj_t *image(const lv_img_dsc_t *source, int x, int y, uint32_t color)
{
    lv_obj_t *obj = lv_img_create(classic_page);
    lv_img_set_src(obj, source);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_img_recolor(obj, lv_color_hex(color), 0);
    lv_obj_set_style_img_recolor_opa(obj, LV_OPA_COVER, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

#if CONFIG_PANEL_BATTERY
/* Plain rectangle on the top layer (used for the battery body, tip, fill). */
static lv_obj_t *box(int x, int y, int width, int height, int radius)
{
    lv_obj_t *obj = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}
#endif

/* Called by LVGL for every tick of the meter while it draws.
 *  - Removes the built-in number labels (part->text). The six numbers are
 *    separate label objects instead, because the ones at the top and the
 *    ones at the bottom needed different radii to clear the ticks.
 *  - Paints ticks 25..50 red. With 51 ticks over 0..RPM_MAX, tick 25 is the
 *    middle of the scale (2500 RPM with the default 5000 full scale), where
 *    the warning arc starts. NOTE: this is fixed at the middle of the scale;
 *    if you move PANEL_RPM_WARNING elsewhere, change the 25 to match. */
static void meter_draw(lv_event_t *event)
{
    lv_obj_draw_part_dsc_t *part = lv_event_get_draw_part_dsc(event);
    if (part->type != LV_METER_DRAW_PART_TICK) return;
    if (part->text) part->text[0] = '\0';
    if (part->line_dsc && part->id >= 25)
        part->line_dsc->color = lv_color_hex(0xbb1e10);
}

/* Centres "850 RPM" as one block: number + 10 px gap + unit. Two fonts are
 * involved, so both labels share the same baseline (y = 312). Called only
 * when the number text changed, because it needs a layout pass. */
static void center_rpm(void)
{
    lv_obj_update_layout(rpm_label);
    lv_obj_update_layout(rpm_unit);
    int width = lv_obj_get_width(rpm_label);
    int left = 240 - (width + 10 + lv_obj_get_width(rpm_unit)) / 2;
    lv_obj_align(rpm_label, LV_ALIGN_TOP_LEFT, left, 312 - panel_rpm_48.line_height + panel_rpm_48.base_line);
    lv_obj_align(rpm_unit, LV_ALIGN_TOP_LEFT, left + width + 10, 312 - panel_unit_22.line_height + panel_unit_22.base_line);
}

/* Applies the colours of the current theme to every object that uses them. */
static void apply_theme(void)
{
    bool paper = theme == 2;
    uint32_t ink = foregrounds[theme];
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(backgrounds[theme]), 0);
    lv_obj_set_style_text_color(lv_scr_act(), lv_color_hex(ink), 0);
    lv_obj_set_style_img_recolor(brand, lv_color_hex(ink), 0);
    lv_obj_set_style_text_color(rpm_label, lv_color_hex(ink), 0);
    lv_obj_set_style_text_color(rpm_unit, lv_color_hex(paper ? 0x69716c : 0x929c9d), 0);
    lv_obj_set_style_opa(meter, theme == 1 ? LV_OPA_40 : LV_OPA_COVER, 0);
    for (int i = 0; i < 6; ++i)
        lv_obj_set_style_text_color(scale_labels[i], lv_color_hex(ink), 0);
    for (int i = 0; i < 3; ++i) {
        uint32_t color = paper ? paper_axis_colors[i] : axis_colors[i];
        lv_obj_set_style_bg_color(axis_cards[i], lv_color_hex(paper ? 0xdedbd2 : 0x182127), 0);
        lv_obj_set_style_border_color(axis_cards[i], lv_color_hex(color), 0);
        lv_obj_set_style_text_color(axis_values[i], lv_color_hex(color), 0);
    }
}

/* Builds the Classic page once at start-up. After this only properties of
 * existing objects change; nothing is created or deleted at run time, so
 * there is no heap churn and no fragmentation in the LVGL pool. */
void dashboard_init(void)
{
    lv_obj_t *screen = classic_page = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(screen,0,0);lv_obj_set_size(screen,480,480);
    lv_obj_set_style_bg_opa(screen,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(screen,0,0);lv_obj_set_style_pad_all(screen,0,0);
    lv_obj_set_style_radius(screen,0,0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(screen, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(screen,lv_color_hex(0xf3f0e8),0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    meter = lv_meter_create(screen);
    lv_obj_set_size(meter, 478, 478);
    lv_obj_center(meter);
    lv_obj_clear_flag(meter, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(meter, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(meter, 1, 0);
    lv_obj_set_style_border_color(meter, lv_color_hex(0x535d60), 0);
    lv_obj_set_style_radius(meter, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(meter, 1, 0);
    lv_obj_add_event_cb(meter, meter_draw, LV_EVENT_DRAW_PART_BEGIN, NULL);
    /* Scale geometry: 0..RPM_MAX over 270 degrees, starting at 135 degrees.
     * LVGL measures angles clockwise from the 3 o'clock direction, so 135
     * is bottom-left (7:30 on a clock) and the end at 135 + 270 = 405 = 45
     * is bottom-right. 51 minor ticks = 50 steps of RPM_MAX/50; every 10th
     * tick is a major tick -> 6 major ticks at 0, 1/5, 2/5 ... of the range. */
    lv_meter_scale_t *scale = lv_meter_add_scale(meter);
    lv_meter_set_scale_ticks(meter, scale, 51, 2, 12, lv_color_hex(0x929c9d));
    lv_meter_set_scale_major_ticks(meter, scale, 10, 3, 24, lv_color_hex(0x929c9d), 35);
    lv_meter_set_scale_range(meter, scale, 0, CONFIG_PANEL_RPM_MAX, 270, 135);
    lv_meter_indicator_t *band = lv_meter_add_arc(meter, scale, 6, lv_color_hex(0xbb1e10), 0);
    lv_meter_set_indicator_start_value(meter, band, CONFIG_PANEL_RPM_WARNING);
    lv_meter_set_indicator_end_value(meter, band, CONFIG_PANEL_RPM_MAX);
    /* Scale numbers. Major tick i sits at 135 + i * 54 degrees (270 / 5 = 54).
     * Position on a circle: x = cx + r cos(a), y = cy + r sin(a), with a in
     * radians (0.01745329252 = pi / 180). The two top numbers (i = 2, 3)
     * use a larger radius, the two bottom ones (i = 0, 5) are pulled up to
     * y radius 171 so they don't touch the axis cards. */
    for (int i = 0; i < 6; ++i) {
        float angle = (135 + i * 54) * 0.01745329252f;
        int radius = (i == 2 || i == 3) ? 198 : 178;
        int x = 240 + (int)lroundf(radius * cosf(angle));
        int y = 240 + (int)lroundf((i == 0 || i == 5 ? 171 : radius) * sinf(angle));
        char number[12];
        snprintf(number, sizeof number, "%d", CONFIG_PANEL_RPM_MAX * i / 5);
        scale_labels[i] = label(number, x, y + 8, &lv_font_montserrat_24);
    }
    brand = image(&panel_brand, 170, 65, 0xf3f0e8);
    image(&panel_emco, 35, 238, 0xbb1e10);
    image(&panel_model, 367, 240, 0xbb1e10);
    status_label = label("NO WIFI", 240, 200, &lv_font_montserrat_18);
    rpm_label = label("----", 240, 312, &panel_rpm_48);
    rpm_unit = label("RPM", 240, 312, &panel_unit_22);
    feed_label = label("F -- mm/min", 240, 348, &panel_text_21);
    for (int i = 0; i < 3; ++i) {
        axis_cards[i] = lv_obj_create(screen);
        lv_obj_set_pos(axis_cards[i], 96 + i * 100, 380);
        lv_obj_set_size(axis_cards[i], 88, 28);
        lv_obj_set_style_radius(axis_cards[i], 4, 0);
        lv_obj_set_style_border_width(axis_cards[i], 1, 0);
        lv_obj_set_style_border_opa(axis_cards[i], LV_OPA_30, 0);
        lv_obj_set_style_pad_all(axis_cards[i], 0, 0);
        lv_obj_clear_flag(axis_cards[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        axis_values[i] = lv_label_create(axis_cards[i]);
        lv_obj_set_style_text_font(axis_values[i], &panel_text_21, 0);
        lv_label_set_text(axis_values[i], "--");
        lv_obj_center(axis_values[i]);
    }
    /* Indicators sit 6 px higher than they used to, leaving room for the
     * battery icon inside the visible circle. */
    icons[0] = image(&panel_spindle_icon, 188, 426, 0x748087);
    icons[1] = image(&panel_coolant_icon, 260, 426, 0x748087);
#if CONFIG_PANEL_BATTERY
    /* Phone style battery, 32x13 px, centred at the bottom. The fill is at
     * most 25 px wide; the bolt is drawn on top while charging. */
    battery_body = box(225, 458, 29, 13, 3);
    lv_obj_set_style_bg_opa(battery_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(battery_body, 1, 0);
    lv_obj_set_style_border_opa(battery_body, LV_OPA_50, 0);
    battery_tip = box(255, 462, 2, 5, 1);
    lv_obj_set_style_bg_opa(battery_tip, LV_OPA_50, 0);
    battery_fill = box(227, 460, 25, 9, 2);
    lv_obj_add_flag(battery_fill, LV_OBJ_FLAG_HIDDEN);
    bolt_outline = image(&panel_bolt_outline, 234, 457, backgrounds[0]);
    bolt = image(&panel_bolt, 234, 457, foregrounds[0]);
    lv_obj_add_flag(bolt_outline, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bolt, LV_OBJ_FLAG_HIDDEN);
#endif
    /* The needle. Created AFTER labels, logos and indicators so it is drawn
     * on top of them. The image is 256 x 40 px, pointing right (0 degrees),
     * with its pivot 22 px from the left edge and 20 px down, i.e. on the
     * hub. Placing the image at (218, 220) puts that pivot exactly at the
     * screen centre (218 + 22, 220 + 20) = (240, 240). */
    pointer = lv_img_create(screen);
    lv_img_set_src(pointer, &panel_needle);
    lv_img_set_pivot(pointer, 22, 20);
    lv_obj_set_pos(pointer, 218, 220);
    lv_obj_clear_flag(pointer, LV_OBJ_FLAG_CLICKABLE);
    hub = lv_obj_create(screen);
    lv_obj_set_size(hub, 24, 24);
    lv_obj_center(hub);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, lv_color_hex(0x293034), 0);
    lv_obj_set_style_border_width(hub, 1, 0);
    lv_obj_set_style_border_color(hub, lv_color_hex(0x8a9293), 0);
    lv_obj_clear_flag(hub, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *center = lv_obj_create(screen);
    lv_obj_set_size(center, 12, 12);
    lv_obj_center(center);
    lv_obj_set_style_radius(center, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(center, lv_color_hex(0xff7800), 0);
    lv_obj_set_style_border_width(center, 0, 0);
    lv_obj_clear_flag(center, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    apply_theme();
    center_rpm();
    pages_init(classic_page,brand);
}

/* Called by the main loop every 40 ms (25 fps) with the latest sample.
 *   fresh  a valid sample arrived within PANEL_STALE_MS
 *   wifi   connected to the access point with an IP address
 * Also forwards the sample to the other pages. */
void dashboard_update(const panel_sample_t *s, bool fresh, bool wifi)
{
    pages_update(s,fresh,wifi);
    static int old_gauge = -1, old_theme = -1, old_usable = -1, old_icons[2] = {-2,-2};
    static int old_axis_present[3] = {-1,-1,-1};
    bool usable = fresh && s->valid;
    const char *status = !wifi ? "NO WIFI" : !fresh ? "NO DATA" : !s->valid ? "WAITING FOR HAL" : s->demo ? "DEMO" : "CONNECTED";
    text_if_changed(status_label, status);
    bool changed_theme = old_theme != theme;
    if (old_usable != usable || changed_theme) {
        lv_obj_set_style_text_color(status_label, lv_color_hex(usable ? (theme == 2 ? 0x247743 : 0x69ac8c) : 0xe88749), 0);
    }
    /* Needle. fabsf: M4 (reverse) gives negative RPM, the gauge shows the
     * magnitude. The value is clamped to full scale and rounded to whole
     * RPM so tiny changes don't rotate the image.
     * lv_img_set_angle() takes tenths of a degree, clockwise from 3 o'clock:
     *   angle = 1350 (135.0 deg, start of scale) + 2700 * rpm / RPM_MAX
     * and % 3600 keeps it in 0..3599 (405.0 deg = 45.0 deg). */
    float rpm = usable && (s->present & PANEL_BIT_RPM) ? fabsf(s->rpm) : 0;
    int gauge = (int)fminf(rpm, CONFIG_PANEL_RPM_MAX);
    if (gauge != old_gauge) {
        lv_img_set_angle(pointer, (1350 + (int)lroundf(2700.0f * gauge / CONFIG_PANEL_RPM_MAX)) % 3600);
        old_gauge = gauge;
    }
    char buffer[96];
    if (usable && (s->present & PANEL_BIT_RPM)) snprintf(buffer, sizeof buffer, "%.0f", (double)rpm);
    else snprintf(buffer, sizeof buffer, "----");
    if (text_if_changed(rpm_label, buffer)) center_rpm();
    /* feed arrives in mm/s (LinuxCNC's motion.current-vel); x60 = mm/min. */
    if (usable && (s->present & PANEL_BIT_FEED)) snprintf(buffer, sizeof buffer, "F %.0f mm/min", (double)(s->feed * 60));
    else snprintf(buffer, sizeof buffer, "F -- mm/min");
    text_if_changed(feed_label, buffer);
    const float coordinates[] = {s->x,s->y,s->z};
    for (int i = 0; i < 3; ++i) {
        bool present = usable && (s->present & PANEL_BIT(PANEL_CH_X + i));
        if (present) snprintf(buffer, sizeof buffer, "%.1f", (double)coordinates[i]);
        else snprintf(buffer, sizeof buffer, "--");
        if (text_if_changed(axis_values[i], buffer)) {
            /* The cards are 88 px wide. Measure the text in the normal
             * font; if it doesn't fit in 76 px (e.g. "-1234.5") fall back
             * to a smaller built-in font instead of clipping digits. */
            lv_point_t size;
            lv_txt_get_size(&size, buffer, &panel_text_21, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            const lv_font_t *font = size.x <= 76 ? &panel_text_21 : strlen(buffer) < 9 ? &lv_font_montserrat_18 : &lv_font_montserrat_14;
            lv_obj_set_style_text_font(axis_values[i], font, 0);
        }
        if (old_axis_present[i] != present || changed_theme)
            lv_obj_set_style_text_color(axis_values[i], lv_color_hex(present ? (theme == 2 ? paper_axis_colors[i] : axis_colors[i]) : 0x748087), 0);
        old_axis_present[i] = present;
    }
    const bool flags[] = {s->spindle_on,s->coolant};
    for (int i = 0; i < 2; ++i) {
        /* -1 unknown (grey), 0 off (red), 1 on (green). */
        int state = usable && (s->present & PANEL_BIT(PANEL_CH_SPINDLE_ON + i)) ? (int)flags[i] : -1;
        if (state != old_icons[i] || changed_theme) {
            uint32_t color = state < 0 ? 0x748087 : state ? (theme == 2 ? 0x247743 : 0x51d88a) : 0xef5350;
            lv_obj_set_style_img_recolor(icons[i], lv_color_hex(color), 0);
            old_icons[i] = state;
        }
    }
    old_theme = theme;
    old_usable = usable;
}

#if CONFIG_PANEL_BATTERY
static void show(lv_obj_t *obj, bool visible)
{
    if (visible) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
#endif

/* Updates the battery icon. Shared by all pages: it lives on the top layer
 * (lv_scr_act) and is raised above the pages the first time it is drawn. */
void dashboard_battery(const battery_status_t *b)
{
#if CONFIG_PANEL_BATTERY
    int theme = pages_white() ? 2 : 0;
    /* Only touch objects when something visible changes: a new percentage
     * that doesn't change the fill width causes no redraw. */
    static int old_width = -1, old_bolt = -1, old_theme = -1;
    static uint32_t old_fill = 0, old_frame = 0;
    static bool raised;
    if (!raised) {
        lv_obj_t *objects[]={battery_body,battery_tip,battery_fill,bolt_outline,bolt};
        for (int i=0;i<5;++i) lv_obj_move_foreground(objects[i]);
        raised=true;
    }
    bool paper = theme == 2;
    uint32_t ink = foregrounds[theme], grey = 0x748087;
    /* Fill width: 0..100 % mapped onto 0..25 px with integer rounding
     * ((p * 25 + 50) / 100 = round(p * 0.25)). Any non-zero charge shows at
     * least 2 px so "almost empty" never looks like "no battery". Because
     * of this quantisation 78 % and 79 % give the same 20 px: no redraw. */
    int width = 0;
    if (b->valid) {
        width = (b->percent * 25 + 50) / 100;
        if (b->percent > 0 && width < 2) width = 2;
    }
    uint32_t fill = !b->valid ? grey : b->charging ? (paper ? 0x247743 : 0x51d88a)
        : b->percent <= 10 ? 0xef5350 : b->percent <= 20 ? (paper ? 0xa86400 : 0xffb547) : ink;
    uint32_t frame = b->valid ? ink : grey;
    int with_bolt = b->valid && b->charging;
    if (frame != old_frame || theme != old_theme) {
        lv_obj_set_style_border_color(battery_body, lv_color_hex(frame), 0);
        lv_obj_set_style_bg_color(battery_tip, lv_color_hex(frame), 0);
        old_frame = frame;
    }
    if (width != old_width) {
        if (width > 0) lv_obj_set_width(battery_fill, width);
        show(battery_fill, width > 0);
        old_width = width;
    }
    if (fill != old_fill || theme != old_theme) {
        lv_obj_set_style_bg_color(battery_fill, lv_color_hex(fill), 0);
        old_fill = fill;
    }
    if (theme != old_theme) {
        lv_obj_set_style_img_recolor(bolt_outline, lv_color_hex(paper?0xf3f0e8:backgrounds[0]), 0);
        lv_obj_set_style_img_recolor(bolt, lv_color_hex(ink), 0);
    }
    if (with_bolt != old_bolt) {
        show(bolt_outline, with_bolt);
        show(bolt, with_bolt);
        old_bolt = with_bolt;
    }
    old_theme = theme;
#else
    (void)b;
#endif
}
