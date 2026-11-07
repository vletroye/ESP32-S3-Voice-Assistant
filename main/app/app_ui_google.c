/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Screen linking the Google account: see app_ui_google.h.
 */

#include <string.h>
#include "esp_log.h"
#include "lvgl.h"
#include "ui.h"
#include "file_lang.h"
#include "google_auth.h"
#include "app_ui_google.h"

LV_FONT_DECLARE(ui_font_Latin14);
LV_FONT_DECLARE(ui_font_Latin20);

/* The consent page's address is long (~280 characters): a large QR code, so that its small squares are read */
#define GOOGLE_QR_SIZE      184
#define GOOGLE_URL_MAX      600

static const char *TAG = "ui_google";

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_qr = NULL;
static lv_obj_t *s_steps = NULL;
static void (*s_done)(void) = NULL;

static void back_cb(lv_event_t *e)
{
    lv_scr_load(ui_ScreenSettings);
    if (s_done) {
        s_done();
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
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 10, 12);

    // The QR code on the left (white around it: phones read it better), the steps on the right
    s_qr = lv_qrcode_create(s_screen, GOOGLE_QR_SIZE, lv_color_hex(0x000000), lv_color_hex(0xFFFFFF));
    lv_obj_set_pos(s_qr, 8, 46);
    lv_obj_set_style_border_color(s_qr, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_qr, 4, LV_PART_MAIN | LV_STATE_DEFAULT);

    s_steps = lv_label_create(s_screen);
    lv_obj_set_pos(s_steps, 8 + GOOGLE_QR_SIZE + 14, 44);
    lv_obj_set_width(s_steps, 320 - (8 + GOOGLE_QR_SIZE + 14) - 4);
    lv_label_set_long_mode(s_steps, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_steps, &ui_font_Latin14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_steps, lv_color_hex(0xB9CCF5), LV_PART_MAIN | LV_STATE_DEFAULT);
}

void ui_google_open(void (*done)(void))
{
    if (!s_screen) {
        screen_create();
    }
    s_done = done;
    const app_lang_t *lang = app_lang_get();
    lv_label_set_text(s_title, lang->google_title);
    lv_label_set_text(s_steps, lang->google_steps);
    static char url[GOOGLE_URL_MAX];
    google_auth_consent_url(url, sizeof(url));
    if (lv_qrcode_update(s_qr, url, strlen(url)) != LV_RES_OK) {
        ESP_LOGE(TAG, "QR code not made (%u characters)", (unsigned)strlen(url));
    }
    lv_scr_load(s_screen);
}
