/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Raúl Milla (DominicZe)
 *
 * Runs the SAME LVGL dashboard code in memory, no SDL and no board. Checks
 * that unchanged data draws nothing, and writes images of every page. */
#include "lvgl.h"
#include "dashboard.h"
#include "pages.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_color_t framebuffer[480*480], buffer[480*20];
static unsigned flushes, pixels;
static lv_point_t touch_point;
static bool touch_pressed;
static void touch_read(lv_indev_drv_t *driver, lv_indev_data_t *data)
{
    (void)driver;
    data->state=touch_pressed?LV_INDEV_STATE_PR:LV_INDEV_STATE_REL;
    data->point=touch_point;
}
/* A real gesture: the indev reports the point and LVGL decides what is hit. */
static void touch(bool pressed, int x, int y)
{
    touch_pressed=pressed;touch_point=(lv_point_t){.x=x,.y=y};
    lv_tick_inc(40);lv_timer_handler();
}
static lv_obj_t *find_slider(lv_obj_t *parent)
{
    if (lv_obj_check_type(parent,&lv_slider_class)) return parent;
    for (unsigned i=0;i<lv_obj_get_child_cnt(parent);++i) {
        lv_obj_t *found=find_slider(lv_obj_get_child(parent,i));if(found)return found;
    }
    return NULL;
}

static lv_obj_t *visible_logo(lv_obj_t *parent)
{
    if (lv_obj_has_flag(parent,LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(parent,&lv_img_class)&&lv_obj_has_flag(parent,LV_OBJ_FLAG_CLICKABLE)) return parent;
    for (unsigned i=0;i<lv_obj_get_child_cnt(parent);++i) {
        lv_obj_t *found=visible_logo(lv_obj_get_child(parent,i));
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find_label(lv_obj_t *parent, const char *text)
{
    for (unsigned i=0;i<lv_obj_get_child_cnt(parent);++i) {
        lv_obj_t *child=lv_obj_get_child(parent,i);
        if (lv_obj_check_type(child,&lv_label_class) && strcmp(lv_label_get_text(child),text)==0) return child;
        lv_obj_t *nested=find_label(child,text);
        if (nested) return nested;
    }
    return NULL;
}

static void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colors)
{
    ++flushes;
    for (int y=area->y1;y<=area->y2;++y)
        for (int x=area->x1;x<=area->x2;++x) {
            framebuffer[y*480+x]=*colors++;
            ++pixels;
        }
    lv_disp_flush_ready(driver);
}

static void save(const char *folder, const char *name)
{
    char path[512];
    snprintf(path,sizeof path,"%s/native-%s.ppm",folder,name);
    FILE *file=fopen(path,"wb");assert(file);
    fprintf(file,"P6\n480 480\n255\n");
    for (int i=0;i<480*480;++i) {
        lv_color32_t color={.full=lv_color_to32(framebuffer[i])};
        unsigned char rgb[]={color.ch.red,color.ch.green,color.ch.blue};
        fwrite(rgb,1,3,file);
    }
    fclose(file);
}

int main(int argc, char **argv)
{
    assert(argc==2);
    lv_init();
    static lv_disp_draw_buf_t draw_buffer;
    lv_disp_draw_buf_init(&draw_buffer,buffer,NULL,480*20);
    static lv_disp_drv_t driver;
    lv_disp_drv_init(&driver);
    driver.hor_res=480;driver.ver_res=480;driver.draw_buf=&draw_buffer;driver.flush_cb=flush;
    lv_disp_t *display=lv_disp_drv_register(&driver);
    static lv_indev_drv_t input;
    lv_indev_drv_init(&input);
    input.type=LV_INDEV_TYPE_POINTER;input.disp=display;input.read_cb=touch_read;
    lv_indev_drv_register(&input);
    dashboard_init();
    panel_sample_t sample={.rpm=850,.feed=650.0f/60,.x=125,.y=-75,.z=-120,
                           .spindle_on=true,.valid=true,.demo=true,.present=127};
    battery_status_t battery={.valid=true,.percent=78,.volts=3.98f};
    dashboard_update(&sample,true,true);
    dashboard_battery(&battery);
    lv_refr_now(display);
    lv_obj_t *rpm=find_label(lv_scr_act(),"850"), *unit=find_label(lv_scr_act(),"RPM");
    assert(rpm && unit);
    lv_obj_t *feed=find_label(lv_scr_act(),"F 650 mm/min");assert(feed);
    assert(lv_color_to32(lv_obj_get_style_text_color(feed,0))==lv_color_to32(lv_color_hex(0xf3f0e8)));
    lv_area_t number_area, unit_area;
    lv_obj_get_coords(rpm,&number_area);lv_obj_get_coords(unit,&unit_area);
    assert(number_area.x1>=100 && unit_area.x2<380);
    assert(abs((number_area.x1+unit_area.x2)/2-240)<=1);
    save(argv[1],"850");
    unsigned previous=flushes;
    for (int i=0;i<25;++i) {
        dashboard_update(&sample,true,true);
        dashboard_battery(&battery);
        lv_tick_inc(40);lv_timer_handler();lv_refr_now(display);
    }
    assert(flushes==previous);
    pixels=0;
    sample.rpm=2500;
    dashboard_update(&sample,true,true);
    lv_refr_now(display);
    assert(pixels>0 && pixels<480*480);
    save(argv[1],"2500");
    unsigned rpm_pixels=pixels;
    /* Battery: 79 % gives the same fill width, so nothing is redrawn. */
    battery.percent=79;
    previous=flushes;
    dashboard_battery(&battery);lv_refr_now(display);
    assert(flushes==previous);
    const struct {int percent; bool charging, valid; const char *name;} states[]={
        {45,true,true,"bat-45-charging"},{15,false,true,"bat-15"},{8,false,true,"bat-8"},
        {100,true,true,"bat-100-usb"},{0,false,false,"bat-no-reading"}};
    for (unsigned i=0;i<sizeof states/sizeof states[0];++i) {
        battery=(battery_status_t){.valid=states[i].valid,.charging=states[i].charging,.percent=states[i].percent};
        pixels=0;
        dashboard_battery(&battery);lv_refr_now(display);
        /* Only the bottom strip changes: body, fill and bolt. */
        assert(pixels>0 && pixels<480*40);
        save(argv[1],states[i].name);
    }
    printf("native LVGL: 25 identical samples, no flush; RPM change=%u of 230400 pixels; battery redraw stays local\n",rpm_pixels);
    sample.demo=false;sample.ui_available=true;sample.control_allowed=true;sample.work_present=true;
    sample.wcs=2;sample.selected_rpm=1500;
    sample.work[0]=25;sample.work[1]=-25;sample.work[2]=-20;
    strcpy(sample.state,"manual");strcpy(sample.token,"1234567890abcdef");
    pages_control_ready(true);
    dashboard_update(&sample,true,true);
    assert(pages_view()==0);
    lv_event_send(lv_scr_act(),LV_EVENT_CLICKED,NULL);
    assert(pages_view()==0); /* tapping the background does nothing */
    lv_obj_t *logo=visible_logo(lv_scr_act());assert(logo);
    lv_event_send(logo,LV_EVENT_PRESSED,NULL);assert(pages_view()==0);
    lv_event_send(logo,LV_EVENT_CLICKED,NULL);assert(pages_view()==1);
    dashboard_update(&sample,true,true);
    battery=(battery_status_t){.valid=true,.percent=78};dashboard_battery(&battery);
    lv_refr_now(display);save(argv[1],"dro");
    previous=flushes;
    for(int i=0;i<25;++i){dashboard_update(&sample,true,true);dashboard_battery(&battery);lv_refr_now(display);}
    assert(flushes==previous);
    assert(find_label(lv_scr_act(),"25.000"));
    lv_obj_t *zero_label=find_label(lv_scr_act(),">0");assert(zero_label);
    lv_event_send(lv_obj_get_parent(zero_label),LV_EVENT_CLICKED,NULL);
    lv_obj_t *title=find_label(lv_scr_act(),"WORK ZERO");assert(title);
    assert(lv_obj_has_flag(lv_obj_get_parent(title),LV_OBJ_FLAG_CLICKABLE));
    lv_obj_t *accept=find_label(lv_scr_act(),"Set zero");assert(accept);
    sample.wcs=3;dashboard_update(&sample,true,true);
    lv_event_send(lv_obj_get_parent(accept),LV_EVENT_CLICKED,NULL);
    panel_command_t zero_command;assert(!pages_take_command(&zero_command));
    sample.wcs=2;dashboard_update(&sample,true,true);
    lv_event_send(visible_logo(lv_scr_act()),LV_EVENT_CLICKED,NULL);assert(pages_view()==2);
    lv_refr_now(display);save(argv[1],"spindle");
    lv_obj_t *plus=find_label(lv_scr_act(),"+100");assert(plus);
    assert(lv_color_to32(lv_obj_get_style_text_color(plus,0))==lv_color_to32(lv_color_hex(0xf3f0e8)));
    lv_event_send(lv_obj_get_parent(plus),LV_EVENT_CLICKED,NULL);
    panel_command_t command;
    assert(pages_take_command(&command)&&!strcmp(command.action,"speed")&&command.rpm==1600);
    assert(!pages_take_command(&command)); /* no duplicate while the finger stays down */
    sample.selected_rpm=1600;dashboard_update(&sample,true,true);pages_result("ok");
    lv_obj_t *slider=find_slider(lv_scr_act());assert(slider);
    /* The knob has its own touch area and dragging is relative: grab it at
     * 1600, move 60 px and 2600 is sent (60*5000/302). */
    int knob=89+1600*302/5000;
    touch(true,knob,280);                    /* grab the knob */
    assert(!pages_take_command(&command));   /* grabbing sends nothing */
    touch(true,knob+60,280);                 /* drag 60 px */
    assert(pages_take_command(&command)&&!strcmp(command.action,"speed")&&command.rpm==2600);
    /* The knob follows the raw value (1600+60*5000/302=2593) while the speed
     * goes in steps of 100, so it doesn't jump 6 px under the finger. */
    assert(lv_slider_get_value(slider)==2593);
    assert(find_label(lv_scr_act(),"2600"));
    sample.selected_rpm=2600;dashboard_update(&sample,true,true);pages_result("ok");
    touch(false,knob+60,280);
    assert(!pages_take_command(&command));
    /* A stale value right after release must not pull the knob back (the
     * "bounce" that showed up when confirming with the spindle running). */
    sample.selected_rpm=2400;dashboard_update(&sample,true,true);
    assert(find_label(lv_scr_act(),"2600"));
    sample.selected_rpm=2600;dashboard_update(&sample,true,true);
    /* Tapping the track away from the knob changes nothing and sends nothing. */
    touch(true,360,280);
    assert(!pages_take_command(&command));
    touch(true,360,280);
    touch(false,360,280);
    assert(!pages_take_command(&command));
    assert(find_label(lv_scr_act(),"2600"));
    /* With a program running the lock screen covers the controls, nothing is sent. */
    knob=89+2600*302/5000;
    touch(true,knob,280);
    sample.control_allowed=false;strcpy(sample.state,"program");dashboard_update(&sample,true,true);
    touch(true,knob+60,280);
    touch(false,knob+60,280);assert(!pages_take_command(&command));
    dashboard_battery(&battery);lv_refr_now(display);assert(pages_white());save(argv[1],"spindle-locked");
    lv_event_send(lv_obj_get_parent(plus),LV_EVENT_CLICKED,NULL);assert(!pages_take_command(&command));
    /* Stop with the spindle turning in a degraded state (override): the button
     * is enabled and the command is queued; the host checks it again. */
    strcpy(sample.state,"override");sample.control_allowed=false;
    sample.spindle_on=true;sample.ui_available=true;sample.demo=false;
    dashboard_update(&sample,true,true);
    lv_obj_t *stop=find_label(lv_scr_act(),"Stop");assert(stop);
    assert(!lv_obj_has_state(lv_obj_get_parent(stop),LV_STATE_DISABLED));
    lv_event_send(lv_obj_get_parent(stop),LV_EVENT_CLICKED,NULL);
    assert(pages_take_command(&command)&&!strcmp(command.action,"stop"));
    lv_event_send(visible_logo(lv_scr_act()),LV_EVENT_CLICKED,NULL);assert(pages_view()==0);
    puts("three LVGL pages: logo navigation, stable DRO, single speed command and program lock OK");
    return 0;
}
