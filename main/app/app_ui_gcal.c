/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Screen linking Google Calendar: see app_ui_gcal.h.
 */

#include <string.h>
#include "esp_log.h"
#include "lvgl.h"
#include "ui.h"
#include "file_lang.h"
#include "google_calendar.h"
#include "app_ui_gcal.h"

LV_FONT_DECLARE(ui_font_Latin14);
LV_FONT_DECLARE(ui_font_Latin20);

#define GCAL_UI_REFRESH_MS  500
#define GCAL_UI_DONE_MS     2500        /* "Agenda connecté !" shown that long, then back to the Settings screen */
#define GCAL_QR_SIZE        130

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_qr = NULL;
static lv_obj_t *s_help = NULL;
static lv_obj_t *s_code = NULL;
static lv_obj_t *s_status = NULL;
static lv_timer_t *s_timer = NULL;
static void (*s_done)(void) = NULL;
static gcal_link_state_t s_shown = GCAL_LINK_IDLE;
static uint32_t s_done_ticks = 0;

static void back(void)
{
    if (s_timer) {
        lv_timer_pause(s_timer);
    }
    lv_scr_load(ui_ScreenSettings);
    if (s_done) {
        s_done();
    }
}

static void back_cb(lv_event_t *e)
{
    const char *url, *code;
    if (gcal_link_state(&url, &code) != GCAL_LINK_DONE) {
        gcal_link_cancel();
    }
    back();
}

static lv_obj_t *label(lv_coord_t x, lv_coord_t y, lv_coord_t w, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(s_screen);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    return l;
}

/* The state shown as it changes: the code once Google gave it, then linked, refused... */
static void refresh_cb(lv_timer_t *t)
{
    const app_lang_t *lang = app_lang_get();
    const char *url, *code;
    gcal_link_state_t state = gcal_link_state(&url, &code);
    if (state == GCAL_LINK_DONE && s_shown == GCAL_LINK_DONE &&
            lv_tick_elaps(s_done_ticks) >= GCAL_UI_DONE_MS) {
        back();
        return;
    }
    if (state == s_shown) {
        return;
    }
    s_shown = state;
    switch (state) {
    case GCAL_LINK_WAITING:
        lv_qrcode_update(s_qr, url, strlen(url));
        lv_obj_clear_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(s_help, lang->gcal_scan, url);
        lv_label_set_text(s_code, code);
        lv_label_set_text(s_status, lang->gcal_waiting);
        break;
    case GCAL_LINK_DONE:
        lv_label_set_text(s_status, lang->gcal_done);
        s_done_ticks = lv_tick_get();
        break;
    case GCAL_LINK_DENIED:
        lv_label_set_text(s_status, lang->gcal_denied);
        break;
    case GCAL_LINK_EXPIRED:
        lv_label_set_text(s_status, lang->gcal_expired);
        break;
    case GCAL_LINK_FAILED:
        lv_label_set_text(s_status, lang->gcal_failed);
        break;
    default:
        lv_label_set_text(s_status, lang->gcal_asking);
        break;
    }
}

static void screen_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_img_src(s_screen, &ui_img_setup_bg_png, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *arrow = lv_img_create(s_screen);
    lv_img_set_src(arrow, &ui_img_settings_back_png);
    lv_obj_set_pos(arrow, 10, 10);
    lv_obj_add_flag(arrow, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_event_cb(arrow, back_cb, LV_EVENT_CLICKED, NULL);

    s_title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_title, &ui_font_Latin20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 14);

    // The QR code on the left (white around it: phones read it better), the code and where it stands on the right
    s_qr = lv_qrcode_create(s_screen, GCAL_QR_SIZE, lv_color_hex(0x000000), lv_color_hex(0xFFFFFF));
    lv_obj_set_pos(s_qr, 14, 62);
    lv_obj_set_style_border_color(s_qr, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_qr, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    s_help = label(162, 52, 150, &ui_font_Latin14, 0xB9CCF5);
    s_code = label(162, 130, 150, &ui_font_Latin20, 0xFFFFFF);
    s_status = label(162, 170, 150, &ui_font_Latin14, 0x3DDC84);
    s_timer = lv_timer_create(refresh_cb, GCAL_UI_REFRESH_MS, NULL);
}

void ui_gcal_open(void (*done)(void))
{
    if (!s_screen) {
        screen_create();
    }
    s_done = done;
    lv_label_set_text(s_title, app_lang_get()->gcal_title);
    lv_label_set_text(s_help, "");
    lv_label_set_text(s_code, "");
    lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
    s_shown = GCAL_LINK_IDLE;
    gcal_link_start();
    refresh_cb(NULL);
    lv_timer_resume(s_timer);
    lv_scr_load(s_screen);
}
