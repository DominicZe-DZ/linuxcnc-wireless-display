/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * Classic / DRO / Spindle pages.
 *
 * The three pages are persistent objects and navigating only hides or shows
 * their containers. A value that didn't change is not written again, so it
 * doesn't invalidate anything. Only the DominicZe logos navigate, on CLICKED.
 *
 *   view 0 Classic  (dashboard.c)   tap logo -> view 1
 *   view 1 DRO                      tap logo -> view 2
 *   view 2 Spindle                  tap logo -> view 0
 *
 * Command flow (one command at a time):
 *
 *   user action --request()/send_speed()--> command + queued=true, pending=true
 *   main.c --pages_take_command()--> signs and sends it
 *   main.c --pages_result("ok"/"busy"/"timeout"...)--> pending=false, message
 *
 * While pending, further speed changes are only remembered (pending_resync)
 * and sent as soon as the ACK arrives, so the spindle keeps following the
 * slider without ever having two commands in the air.
 *
 * Spindle slider geometry: the track is 302 px wide starting at x = 89 and
 * maps 0..5000 RPM, so 1 px = 5000/302 = 16.6 RPM. The knob position for a
 * speed is 89 + rpm * 302 / 5000. Speeds are rounded to steps of 100 RPM.
 */
#include "pages.h"
#include "header_assets.h"
#include "dz_logo.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
LV_FONT_DECLARE(panel_digits_42);
LV_FONT_DECLARE(panel_digits_66);

static lv_obj_t *views[3], *logos[3], *work_button, *machine_button, *coordinate;
static lv_obj_t *grab; /* touch area that follows the slider knob */
static lv_obj_t *values[3], *zero[3], *zero_all, *dro_status;
static lv_obj_t *controls, *selection, *spindle_status, *feedback, *slider;
static lv_obj_t *minus_button, *plus_button, *start_button, *stop_button;
static lv_obj_t *block, *block_title, *block_reason, *dialog;
static panel_sample_t sample;
static bool fresh, wifi, ready, machine_coordinates, pending, queued;
static bool white, dragging;
static bool feedback_active;
static int feedback_view;
static bool pending_resync;
static bool stop_queued;   /* Stop pressed while another command was in flight */
static bool ota_showing;   /* update screen is up, nothing gets sent */
static uint32_t resync_ms; /* age of the pending resend, expires after 1.5 s */
static uint32_t last_speed_ms;
static int last_sent_rpm = -1;
static int drag_from_x, drag_from_rpm; /* where the drag started */
static int drag_rpm;  /* raw value under the finger, the knob follows it smoothly */
static char last_feedback[80];
static int view, selected = 1500;
static int confirm_wcs;
static panel_command_t command;
static const uint32_t colors[] = {0x8cdef2,0xa1e3b4,0xffd08a};

/* --- small helpers: each one only touches LVGL when something changes ----- */

/* Set label text only if different (see text_if_changed in dashboard.c). */
static void text(lv_obj_t *object, const char *value)
{
    if (strcmp(lv_label_get_text(object),value)) lv_label_set_text(object,value);
}
/* Show/hide via the HIDDEN flag, only when the state really changes. */
static void visible(lv_obj_t *object, bool show)
{
    if (show && lv_obj_has_flag(object,LV_OBJ_FLAG_HIDDEN)) lv_obj_clear_flag(object,LV_OBJ_FLAG_HIDDEN);
    if (!show && !lv_obj_has_flag(object,LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(object,LV_OBJ_FLAG_HIDDEN);
}
/* Enable/disable (greyed out at 40 % opacity), only on change. */
static void enabled(lv_obj_t *object, bool allow)
{
    if (allow && lv_obj_has_state(object,LV_STATE_DISABLED)) lv_obj_clear_state(object,LV_STATE_DISABLED);
    if (!allow && !lv_obj_has_state(object,LV_STATE_DISABLED)) lv_obj_add_state(object,LV_STATE_DISABLED);
}
/* Plain filled rectangle, not clickable, not scrollable. */
static lv_obj_t *box(lv_obj_t *parent,int x,int y,int w,int h,uint32_t color)
{
    lv_obj_t *o=lv_obj_create(parent);
    lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_color(o,lv_color_hex(color),0);
    lv_obj_set_style_border_width(o,0,0);lv_obj_set_style_pad_all(o,0,0);
    lv_obj_set_style_radius(o,0,0);lv_obj_set_style_shadow_width(o,0,0);
    lv_obj_clear_flag(o,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
    return o;
}
/* Centred label of fixed width at (x, y) = top-left corner. */
static lv_obj_t *label(lv_obj_t *parent,const char *value,int x,int y,int width,const lv_font_t *font,uint32_t color)
{
    lv_obj_t *o=lv_label_create(parent);
    lv_obj_set_style_text_font(o,font,0);lv_obj_set_style_text_color(o,lv_color_hex(color),0);
    lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_CENTER,0);lv_label_set_text(o,value);
    lv_obj_set_width(o,width);lv_obj_set_pos(o,x,y);
    return o;
}
/* Dark rounded button with a centred caption. Child 0 is the caption label. */
static lv_obj_t *button(lv_obj_t *parent,const char *value,int x,int y,int width,int height)
{
    lv_obj_t *o=lv_btn_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o,x,y);lv_obj_set_size(o,width,height);
    lv_obj_set_style_radius(o,8,0);lv_obj_set_style_shadow_width(o,0,0);
    lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    lv_obj_set_style_bg_color(o,lv_color_hex(0x192226),0);
    lv_obj_set_style_border_color(o,lv_color_hex(0x425156),0);lv_obj_set_style_border_width(o,1,0);
    lv_obj_set_style_bg_opa(o,LV_OPA_40,LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(o,LV_OPA_40,LV_STATE_DISABLED);
    lv_obj_t *l=lv_label_create(o);lv_label_set_text(l,value);lv_obj_center(l);
    lv_obj_set_style_text_font(l,&lv_font_montserrat_18,0);
    lv_obj_set_style_text_color(l,lv_color_hex(0xf3f0e8),0);
    return o;
}

/* Commands may be sent: fresh valid data, not demo, paired, the PC sent a
 * ui object, it says control is allowed, and nothing is in flight. */
static bool allowed(void)
{
    return fresh && sample.valid && !sample.demo && ready && sample.ui_available
        && sample.control_allowed && !pending;
}
/* Changing the local selection needs less than sending commands: a paired
 * channel, fresh data and not demo mode. While a command is in flight the
 * number keeps moving and the latest value is sent afterwards if needed. */
static bool selection_ready(void)
{
    return fresh && sample.valid && !sample.demo && ready && sample.ui_available;
}
/* Why controls are locked, in priority order (first match wins). */
static const char *reason(void)
{
    if (!wifi) return "NO WIFI";
    if (!fresh || !sample.valid) return "NO DATA";
    if (sample.demo) return "DEMO DATA";
    if (!sample.ui_available) return "UPDATE THE HOST SOFTWARE";
    if (!strcmp(sample.state,"program")) return "Program running";
    if (!strcmp(sample.state,"paused")) return "Program paused";
    if (!strcmp(sample.state,"off")) return "Machine is off";
    if (!ready) return "CONTROL NOT PAIRED";
    if (!strcmp(sample.state,"home")) return "AXES NOT HOMED";
    if (!strcmp(sample.state,"override")) return "SPINDLE OVERRIDE NOT 100%";
    if (!strcmp(sample.state,"css")) return "CSS (G96) ACTIVE";
    return "ONLY IN IDLE MANUAL MODE";
}
static void refresh(void);

/* Logo tapped: hide the current page, show the next one (0 -> 1 -> 2 -> 0). */
static void next(lv_event_t *event)
{
    (void)event;
    // Nothing else bubbles a navigation click up to the background.
    visible(views[view],false);view=(view+1)%3;visible(views[view],true);
    refresh();
}
/* Common header of the DRO and Spindle pages: clickable DominicZe logo,
 * EMCO and F1-CNC marks in red, and the page title. */
static void header(int index)
{
    logos[index]=lv_img_create(views[index]);lv_img_set_src(logos[index],&dz_logo);
    lv_obj_set_style_img_recolor(logos[index],lv_color_hex(0xf3f0e8),0);
    lv_obj_set_style_img_recolor_opa(logos[index],LV_OPA_COVER,0);
    lv_obj_set_pos(logos[index],145,34);lv_obj_add_flag(logos[index],LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(logos[index],4);
    lv_obj_add_event_cb(logos[index],next,LV_EVENT_CLICKED,NULL);
    lv_obj_t *emco=lv_img_create(views[index]);lv_img_set_src(emco,&page_emco);
    lv_obj_set_pos(emco,82,91);
    lv_obj_set_style_img_recolor(emco,lv_color_hex(0xbb1e10),0);lv_obj_set_style_img_recolor_opa(emco,LV_OPA_COVER,0);
    lv_obj_clear_flag(emco,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *model=lv_img_create(views[index]);lv_img_set_src(model,&page_model);
    lv_obj_set_pos(model,339,91);
    lv_obj_set_style_img_recolor(model,lv_color_hex(0xbb1e10),0);lv_obj_set_style_img_recolor_opa(model,LV_OPA_COVER,0);
    lv_obj_clear_flag(model,LV_OBJ_FLAG_CLICKABLE);
    label(views[index],index==1?"DRO":"SPINDLE",170,94,140,&lv_font_montserrat_14,0xadbabb);
}
/* Speed command while dragging. No intermediate feedback text so the screen
 * doesn't flicker; the final result is shown on release. */
static void send_speed(void)
{
    command=(panel_command_t){.rpm=selected,.wcs=sample.wcs};
    pending_resync=false;
    last_speed_ms=lv_tick_get();last_sent_rpm=selected;
    snprintf(command.action,sizeof command.action,"speed");
    snprintf(command.axis,sizeof command.axis,"-");
    queued=pending=true;feedback_view=2;
}
/* Stop doesn't wait for the control permission: if the spindle turns it is
 * always attempted (the host checks again and answers with the reason).
 * Everything else needs the permission. */
static bool permitted(const char *action)
{
    if (!strcmp(action,"stop"))
        return fresh && sample.valid && !sample.demo && ready && sample.ui_available && !pending;
    return allowed();
}
/* Resend the latest value without waiting for the next sample. The sample
 * that came with the ACK may still show the permission taken by the command
 * that just finished, so manual/mode/remote are all fine (see main.c). */
static bool resync_allowed(void)
{
    return fresh && sample.valid && !sample.demo && ready && sample.ui_available && !pending
        && !ota_showing
        && (!strcmp(sample.state,"manual") || !strcmp(sample.state,"remote")
            || !strcmp(sample.state,"mode"));
}
/* Queues one command for main.c, if permitted. */
static void request(const char *action,const char *axis)
{
    if (!permitted(action)) {
        /* A Stop pressed during another command isn't lost: it goes out as
         * soon as the channel is free (the host has a single worker). */
        if (!strcmp(action,"stop") && pending && fresh && sample.valid
                && !sample.demo && ready && sample.ui_available) stop_queued=true;
        return;
    }
    /* A new speed IS sent with the spindle stopped: the host stores it and
     * runs no MDI (result `stopped`). Keeping it only on the screen didn't
     * work, the next sample overwrote it with the old modal S word. */
    command=(panel_command_t){.rpm=selected,.wcs=sample.wcs};
    pending_resync=false;
    last_speed_ms=lv_tick_get();last_sent_rpm=selected;
    snprintf(command.action,sizeof command.action,"%s",action);
    snprintf(command.axis,sizeof command.axis,"%s",axis);
    queued=pending=true;feedback_active=false;
    feedback_view=!strcmp(action,"zero")?1:2;
    text(feedback,"WAITING FOR CONFIRMATION");refresh();
}

/* WORK / MACHINE buttons on the DRO page (user data 0 = work, 1 = machine). */
static void coordinate_click(lv_event_t *event)
{
    machine_coordinates=(intptr_t)lv_event_get_user_data(event)!=0;
    refresh();
}
static void cancel_zero(lv_event_t *event)
{
    (void)event;visible(dialog,false);
}
/* "Set zero" in the dialog. Refused if the active WCS changed while the
 * dialog was open: the user confirmed a zero for G55, we must not set G56. */
static void confirm_zero(lv_event_t *event)
{
    const char *axis=lv_event_get_user_data(event);
    visible(dialog,false);
    if (sample.wcs!=confirm_wcs) {
        /* Local result. Don't go through pages_result(), it would act on the
         * previous command and could resend a speed. */
        feedback_view=1;
        snprintf(last_feedback,sizeof last_feedback,"REJECTED: WORK OFFSET CHANGED");
        feedback_active=true;refresh();return;
    }
    request("zero",axis);
}
/* ">0" or "XYZ > 0" tapped: opens the modal confirmation dialog, which
 * remembers the WCS shown at that moment. */
static void ask_zero(lv_event_t *event)
{
    if (!allowed() || machine_coordinates || !sample.work_present) return;
    const char *axis=lv_event_get_user_data(event);
    confirm_wcs=sample.wcs;
    lv_obj_clean(dialog);
    char title[48];snprintf(title,sizeof title,"Zero %s?",axis);
    label(dialog,"WORK ZERO",100,130,280,&lv_font_montserrat_18,0x8cdef2);
    label(dialog,title,50,170,380,&lv_font_montserrat_24,0xf3f0e8);
    snprintf(title,sizeof title,"Coordinate system %s",sample.wcs<=6?(const char*[]){"","G54","G55","G56","G57","G58","G59"}[sample.wcs]:"G59.x");
    label(dialog,title,90,212,300,&lv_font_montserrat_18,0xf3f0e8);
    label(dialog,"LinuxCNC sets the origin\nat the current position.\nThe axes don't move.",70,246,340,&lv_font_montserrat_18,0xaab4b5);
    lv_obj_t *cancel=button(dialog,"Cancel",100,330,130,48),*accept=button(dialog,"Set zero",250,330,130,48);
    lv_obj_add_event_cb(cancel,cancel_zero,LV_EVENT_CLICKED,NULL);
    lv_obj_add_event_cb(accept,confirm_zero,LV_EVENT_CLICKED,(void*)axis);
    visible(dialog,true);
}
/* Touch events of the slider knob area: PRESSED (grab), PRESSING (drag,
 * repeated while the finger moves), RELEASED and PRESS_LOST (finger slid
 * off or the object got hidden). */
static void speed_event(lv_event_t *event)
{
    lv_event_code_t code=lv_event_get_code(event);
    if (code==LV_EVENT_PRESS_LOST) {   /* gesture interrupted, let go of the knob */
        if (dragging) {
            dragging=false;
            /* Keep the last value under the finger: send it or queue it,
             * same as a normal release. */
            if (resync_allowed()) {
                if (!pending && selected!=last_sent_rpm) send_speed();
                else if (pending) {pending_resync=true;resync_ms=lv_tick_get();}
            }
            refresh();
        }
        return;
    }
    if (code==LV_EVENT_RELEASED) {
        /* Releasing always ends the gesture, even if the data went stale
         * halfway. Otherwise `dragging` would get stuck. */
        if (!dragging) return;
        dragging=false;
        if (allowed()) {
            if (!pending && selected!=last_sent_rpm) send_speed();
            else if (pending) {pending_resync=true;resync_ms=lv_tick_get();}
        }
        if (feedback_view==2 && last_feedback[0]) text(feedback,last_feedback);
        refresh();
        return;
    }
    if (!selection_ready()) return;
    if (code==LV_EVENT_PRESSED) {
        lv_indev_t *indev=lv_event_get_indev(event);
        lv_point_t point;
        if (!indev) return;
        lv_indev_get_point(indev,&point);
        dragging=true;drag_from_x=point.x;drag_from_rpm=selected;drag_rpm=selected;
        last_speed_ms=lv_tick_get()-200;  /* first move is sent right away */
        return;
    }
    if (code==LV_EVENT_PRESSING && dragging) {
        lv_indev_t *indev=lv_event_get_indev(event);
        lv_point_t point;
        if (!indev) return;
        lv_indev_get_point(indev,&point);
        /* Relative drag from where the knob was grabbed, so the value never
         * jumps and the full 0-5000 range follows the finger. */
        int raw=drag_from_rpm+(point.x-drag_from_x)*5000/302;
        if (raw<0) raw=0; else if (raw>5000) raw=5000;
        drag_rpm=raw;  /* knob follows the finger pixel by pixel */
        lv_slider_set_value(slider,drag_rpm,LV_ANIM_OFF);
        int rpm=((raw+50)/100)*100;
        if (rpm==selected) return;
        selected=rpm;
        char number[12];snprintf(number,sizeof number,"%d",selected);text(selection,number);
        /* With a command in flight just remember the latest value; it is
         * sent when the ACK arrives so the spindle keeps following. */
        if (pending) {pending_resync=true;resync_ms=lv_tick_get();return;}
        if (!allowed()) return;
        /* Live tracking: one command every 200 ms with the current value,
         * 0 included (dragging down slows the spindle, 0 stops it). The
         * actual acceleration is whatever the spindle drive ramp allows. */
        uint32_t now=lv_tick_get();
        if (selected!=last_sent_rpm && now-last_speed_ms>=200) send_speed();
        return;
    }
}
/* -100 / +100 buttons (user data = step), clamped to 0..5000. */
static void speed_step(lv_event_t *event)
{
    if (!selection_ready()) return;
    int value=selected+(int)(intptr_t)lv_event_get_user_data(event);
    if (value<0) value=0;
    if (value>5000) value=5000;
    if (value==selected) return;
    selected=value;
    if (allowed() && !pending) request("speed","-");
    else if (pending) {pending_resync=true;refresh();}
    else refresh();
}
static void run_stop(lv_event_t *event)
{
    request(lv_event_get_user_data(event),"-");
}

void pages_init(lv_obj_t *classic,lv_obj_t *classic_logo)
{
    views[0]=classic;logos[0]=classic_logo;
    lv_obj_add_flag(classic_logo,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(classic_logo,next,LV_EVENT_CLICKED,NULL);
    for (int i=1;i<3;++i) {
        views[i]=box(lv_scr_act(),0,0,480,480,0x101519);header(i);visible(views[i],false);
    }
    work_button=button(views[1],"WORK",104,122,135,34);
    machine_button=button(views[1],"MACHINE",247,122,129,34);
    lv_obj_add_event_cb(work_button,coordinate_click,LV_EVENT_CLICKED,(void*)0);
    lv_obj_add_event_cb(machine_button,coordinate_click,LV_EVENT_CLICKED,(void*)1);
    for (int i=0;i<3;++i) {
        int x=i==1?44:56,w=i==1?392:368,y=170+i*75;
        lv_obj_t *row=box(views[1],x,y,w,64,0x182127);
        lv_obj_set_style_radius(row,8,0);lv_obj_set_style_border_width(row,1,0);
        lv_obj_set_style_border_opa(row,LV_OPA_30,0);lv_obj_set_style_border_color(row,lv_color_hex(colors[i]),0);
        label(row,(const char*[]){"X","Y","Z"}[i],12,20,28,&lv_font_montserrat_24,colors[i]);
        values[i]=label(row,"---",45,5,w-105,&panel_digits_42,colors[i]);
        lv_obj_set_style_text_align(values[i],LV_TEXT_ALIGN_RIGHT,0);
        zero[i]=button(row,">0",w-54,9,44,46);
        lv_obj_add_event_cb(zero[i],ask_zero,LV_EVENT_CLICKED,(void*)(const char*[]){"x","y","z"}[i]);
    }
    label(views[1],"mm",127,405,30,&lv_font_montserrat_14,0xa5b4b8);
    zero_all=button(views[1],"XYZ > 0",179,396,110,34);
    lv_obj_add_event_cb(zero_all,ask_zero,LV_EVENT_CLICKED,"xyz");
    coordinate=label(views[1],"--",309,405,70,&lv_font_montserrat_14,0xa5b4b8);
    dro_status=label(views[1],"NO DATA",75,439,330,&lv_font_montserrat_14,0xa1e3b4);
    controls=box(views[2],0,120,480,337,0x101519);
    label(controls,"SET SPEED",140,13,200,&lv_font_montserrat_14,0x9aa9ac);
    selection=label(controls,"1500",65,33,350,&panel_digits_66,0xf3f0e8);
    spindle_status=label(controls,"RPM",40,112,400,&lv_font_montserrat_18,0xa1e3b4);
    slider=lv_slider_create(controls);lv_obj_set_pos(slider,89,156);lv_obj_set_size(slider,302,8);
    lv_slider_set_range(slider,0,5000);lv_slider_set_value(slider,selected,LV_ANIM_OFF);
    /* The track itself ignores touches, so tapping away from the knob never
     * makes the value jump. Only the knob touch area is active. */
    lv_obj_clear_flag(slider,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(slider,lv_color_hex(0xff7800),LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider,lv_color_hex(0xff7800),LV_PART_KNOB);
    grab=lv_obj_create(controls);lv_obj_remove_style_all(grab);
    lv_obj_set_size(grab,90,44);lv_obj_set_pos(grab,44,138);
    lv_obj_add_flag(grab,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(grab,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(grab,speed_event,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(grab,speed_event,LV_EVENT_PRESSING,NULL);
    lv_obj_add_event_cb(grab,speed_event,LV_EVENT_RELEASED,NULL);
    lv_obj_add_event_cb(grab,speed_event,LV_EVENT_PRESS_LOST,NULL);
    label(controls,"0",83,179,30,&lv_font_montserrat_14,0x9aa9ac);
    label(controls,"5000",350,179,50,&lv_font_montserrat_14,0x9aa9ac);
    minus_button=button(controls,"-100",83,203,149,44);plus_button=button(controls,"+100",248,203,149,44);
    lv_obj_add_event_cb(minus_button,speed_step,LV_EVENT_CLICKED,(void*)(intptr_t)-100);
    lv_obj_add_event_cb(plus_button,speed_step,LV_EVENT_CLICKED,(void*)(intptr_t)100);
    start_button=button(controls,"Start",83,249,149,48);stop_button=button(controls,"Stop",248,249,149,48);
    lv_obj_set_style_bg_color(start_button,lv_color_hex(0x163d2b),0);
    lv_obj_set_style_border_color(start_button,lv_color_hex(0x51d88a),0);
    lv_obj_set_style_bg_color(stop_button,lv_color_hex(0x421b18),0);
    lv_obj_set_style_border_color(stop_button,lv_color_hex(0xd04b3e),0);
    lv_obj_add_event_cb(start_button,run_stop,LV_EVENT_CLICKED,"start");lv_obj_add_event_cb(stop_button,run_stop,LV_EVENT_CLICKED,"stop");
    feedback=label(controls,"",60,319,360,&lv_font_montserrat_14,0xa1e3b4);
    block=box(views[2],0,0,480,480,0xf3f0e8);
    block_title=label(block,"",50,185,380,&lv_font_montserrat_32,0x202b2b);
    block_reason=label(block,"",50,282,380,&lv_font_montserrat_18,0x4b5659);
    label(block,"Tap DominicZe to go back\nto the Classic gauge.",60,338,360,&lv_font_montserrat_14,0x657174);
    lv_obj_move_foreground(logos[2]);
    dialog=box(views[1],0,0,480,480,0x101519);
    lv_obj_add_flag(dialog,LV_OBJ_FLAG_CLICKABLE); /* modal, blocks the controls underneath */
    visible(dialog,false);
    refresh();
}

/* Brings every object of the DRO and Spindle pages in line with the current
 * sample and command state. Called after every sample and every event; the
 * helpers above make it cheap when nothing changed. */
static void refresh(void)
{
    if (!controls) return;
    bool usable=fresh&&sample.valid, permit=allowed();
    /* "Stable" permission for drawing: a running command puts the state in
     * remote/mode for ~130 ms and the buttons must not flicker because of it. */
    bool stable=permit||pending||!strcmp(sample.state,"remote")||!strcmp(sample.state,"mode");
    /* The white lock screen only covers states where nothing can be done: no
     * data, demo, old host, unpaired control, machine off or a program
     * running. Transient states (MDI, busy, override, not homed) show the
     * controls greyed out with the reason, and Stop stays available while
     * the spindle is really turning. */
    bool was_white=white;
    bool blocked=white=view==2 && (!usable||!ready||!sample.ui_available||sample.demo
        ||!strcmp(sample.state,"unknown")||!strcmp(sample.state,"off")
        ||!strcmp(sample.state,"program")||!strcmp(sample.state,"paused"));
    if (!lv_obj_has_flag(dialog,LV_OBJ_FLAG_HIDDEN)&&sample.wcs!=confirm_wcs) visible(dialog,false);
    char value[80],wcs[16];
    /* LinuxCNC numbers the work systems 1..9: 1..6 = G54..G59 (so G = 53 +
     * n), 7..9 = G59.1..G59.3. */
    if (sample.wcs>=1 && sample.wcs<=6) snprintf(wcs,sizeof wcs,"G%d",sample.wcs+53);
    else if (sample.wcs>=7 && sample.wcs<=9) snprintf(wcs,sizeof wcs,"G59.%d",sample.wcs-6);
    else snprintf(wcs,sizeof wcs,"--");
    snprintf(value,sizeof value,"WORK %s",wcs);text(lv_obj_get_child(work_button,0),value);
    text(coordinate,machine_coordinates?"MACH.":wcs);
    uint32_t selected_color=machine_coordinates?0x192226:0x2b3d42;
    if (lv_color_to32(lv_obj_get_style_bg_color(work_button,0))!=lv_color_to32(lv_color_hex(selected_color))) {
        lv_obj_set_style_bg_color(work_button,lv_color_hex(selected_color),0);
        lv_obj_set_style_bg_color(machine_button,lv_color_hex(machine_coordinates?0x2b3d42:0x192226),0);
    }
    const float machine[]={sample.x,sample.y,sample.z};
    for (int i=0;i<3;++i) {
        bool present=usable&&(machine_coordinates?(sample.present&PANEL_BIT(PANEL_CH_X+i)):sample.work_present);
        if (present) snprintf(value,sizeof value,"%.3f",(double)(machine_coordinates?machine[i]:sample.work[i]));
        else snprintf(value,sizeof value,"---");
        text(values[i],value);visible(zero[i],!machine_coordinates);enabled(zero[i],stable&&sample.work_present);
    }
    visible(zero_all,!machine_coordinates);enabled(zero_all,stable&&sample.work_present);
    text(dro_status,pending&&feedback_view==1?"WAITING FOR CONFIRMATION":feedback_active&&feedback_view==1?last_feedback:stable?"CONNECTED":reason());
    if (!permit && !pending) visible(dialog,false);
    white=blocked;
    visible(block,white);visible(controls,!white);
    if (white) {
        bool program=!strcmp(sample.state,"program")||!strcmp(sample.state,"paused");
        bool off=!strcmp(sample.state,"off");
        text(block_title,program?"Machine is\nrunning a job":off?"Machine\nis off":"Control not\navailable");
        text(block_reason,off?"Release the E-stop and home the axes":reason());
    }
    if (was_white!=white) {
        lv_obj_set_style_img_recolor(logos[2],lv_color_hex(white?0x202b2b:0xf3f0e8),0);
        lv_obj_set_style_img_recolor_opa(logos[2],LV_OPA_COVER,0);
    }
    /* The finger wins over incoming data: while dragging, the number and the
     * knob belong to the user and refresh() leaves them alone. */
    if (!dragging&&!pending&&sample.ui_available
            && lv_tick_get()-last_speed_ms>400) selected=sample.selected_rpm;
    snprintf(value,sizeof value,"%d",selected);text(selection,value);
    if (!dragging&&lv_slider_get_value(slider)!=selected) lv_slider_set_value(slider,selected,LV_ANIM_OFF);
    snprintf(value,sizeof value,"RPM  -  %s",sample.spindle_on?"RUNNING":"STOPPED");text(spindle_status,value);
    bool adjust=selection_ready();  /* the local value never locks while waiting */
    enabled(slider,adjust);enabled(minus_button,adjust);enabled(plus_button,adjust);
    /* Move the knob touch area along with the value. */
    int knob_x = 89 + (dragging?drag_rpm:selected) * 302 / 5000;
    lv_obj_set_pos(grab,knob_x-45,138);
    enabled(start_button,stable&&selected>0&&!sample.spindle_on);
    /* Stop only needs fresh data and a spindle that is really turning. If the
     * machine refuses, the host answers with the reason and it is shown;
     * there is never a greyed out Stop without explanation. */
    enabled(stop_button,usable&&!sample.demo&&ready&&sample.ui_available&&sample.spindle_on);
    /* Pending value from a drag: send it with this sample's fresh permission.
     * If commands aren't allowed it expires after 1.5 s, so an old speed is
     * never left waiting for the machine to become free. */
    if (pending_resync) {
        if (allowed() && !pending && selected!=last_sent_rpm
                && lv_tick_get()-last_speed_ms>=200) send_speed();
        else if (lv_tick_get()-resync_ms>=1500) pending_resync=false;
    }
}
void pages_update(const panel_sample_t *s,bool f,bool w)
{
    sample=*s;fresh=f;wifi=w;refresh();
}
void pages_control_ready(bool value) {ready=value;refresh();}
bool pages_take_command(panel_command_t *out)
{
    if (!queued) return false;
    queued=false;*out=command;return true;
}
void pages_result(const char *result)
{
    if (!strcmp(result,"pending")) return;
    bool had_resync=pending_resync;
    pending=false;
    const char *timeout_message=!strcmp(command.action,"speed")
        ? "NOT CONFIRMED - RETRYING" : "NOT CONFIRMED - NOT RESENT";
    const char *message=!strcmp(result,"ok")?"CONFIRMED BY LINUXCNC":
        !strcmp(result,"timeout")?timeout_message:
        !strcmp(result,"program")?"REJECTED: PROGRAM RUNNING":
        !strcmp(result,"paused")?"REJECTED: PROGRAM PAUSED":
        !strcmp(result,"wcs")?"REJECTED: WORK OFFSET CHANGED":
        !strcmp(result,"expired")?"REJECTED: REQUEST EXPIRED":
        !strcmp(result,"unconfirmed")?"RESULT NOT CONFIRMED":
        !strcmp(result,"stopped")?"SPEED SAVED - APPLIED ON START":
        !strcmp(result,"busy")?"BUSY - ANOTHER COMMAND RUNNING":
        !strcmp(result,"mode")?"REJECTED: NOT IN MANUAL MODE":
        !strcmp(result,"home")?"REJECTED: AXES NOT HOMED":
        !strcmp(result,"css")?"REJECTED: CSS (G96) ACTIVE":
        !strcmp(result,"override")?"REJECTED: OVERRIDE NOT 100%":
        !strcmp(result,"off")?"REJECTED: MACHINE IS OFF":
        !strcmp(result,"dropped")?"NOT SENT - WRONG STATE":
        "DONE - PLEASE CHECK THE STATE";
    snprintf(last_feedback,sizeof last_feedback,"%s",message);
    /* Don't print every ACK while dragging, it would just flicker. The
     * result shows up on release (including "speed saved" when stopped). */
    if (!dragging) {
        feedback_active=true;
        if (feedback_view==2) text(feedback,message);
    }
    if (stop_queued) {
        stop_queued=false;
        if (permitted("stop")) { request("stop","-"); return; }
    }
    /* The drag went on while the command was in flight, so the local value
     * is newer than the one just confirmed. Send it now (main.c accepts the
     * remote state left by the ACK) so the spindle keeps following the
     * finger at roughly one block every 130 ms. If it can't be sent now,
     * refresh() retries with the next fresh sample. */
    if (!strcmp(command.action,"speed")) {
        bool accepted=!strcmp(result,"ok")||!strcmp(result,"stopped");
        if (!accepted) {
            /* Dropped before it went out (e.g. sample not settled yet).
             * Keep the local value and retry with the next sample. */
            last_sent_rpm=-1;
            pending_resync=true;
        } else {
            pending_resync=had_resync && selected!=command.rpm;
        }
        if (pending_resync) {
            resync_ms=lv_tick_get();
            /* Only resend with a valid control state. Resending blindly
             * caused a 200 Hz reject loop with `pending` stuck, which also
             * killed the Stop button. refresh() retries later otherwise. */
            if (resync_allowed()) send_speed();
        }
    } else {
        pending_resync=false;
    }
    refresh();
}

/* Update screen: replaces the Spindle page with the progress and blocks every
 * control until the reboot or the failure. */

void pages_ota(panel_upd_state_t state, int percent)
{
    ota_showing=state!=PANEL_UPD_IDLE;
    if (!ota_showing) {visible(views[2],false);return;}
    for (int i=0;i<3;++i) visible(views[i],i==2);  /* only one page */
    lv_obj_move_foreground(views[2]);
    const char *title=state==PANEL_UPD_FAIL?"Update\nnot completed":
        state==PANEL_UPD_DONE?"Update\nverified":"Updating\nthe display";
    text(block_title,title);
    static char ota_line[80];
    if (state==PANEL_UPD_FAIL) snprintf(ota_line,sizeof ota_line,"Nothing changed, still on the old version");
    else if (state==PANEL_UPD_DONE) snprintf(ota_line,sizeof ota_line,"Restarting in a moment...");
    else snprintf(ota_line,sizeof ota_line,"Download and check: %d%%",percent<0?0:percent);
    text(block_reason,ota_line);
    visible(block,true);visible(controls,false);
    lv_obj_move_foreground(block);
}

void pages_ota_end(void)
{
    if (!ota_showing) return;
    ota_showing=false;
    for (int i=0;i<3;++i) visible(views[i],i==view);
    visible(block,false);visible(controls,true);
    refresh();
}
int pages_view(void) {return view;}
bool pages_white(void) {return white;}
