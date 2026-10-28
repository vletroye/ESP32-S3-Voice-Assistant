/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "ui.h"
#include "app_lang.h"
#include "app_volume.h"
#include "settings.h"
#include "radio.h"
#include "radio_favorites.h"
#include "app_ui_radio.h"

LV_FONT_DECLARE(ui_font_Latin14);
LV_FONT_DECLARE(ui_font_Latin16);
LV_FONT_DECLARE(ui_font_Latin20);
LV_FONT_DECLARE(ui_font_Radio28);
LV_FONT_DECLARE(ui_font_Note22);

/* Symbols of ui_font_Radio28 */
#define SYM_HEART_EMPTY     "\xE2\x99\xA1"      /* U+2661 */
#define SYM_HEART_FULL      "\xE2\x99\xA5"      /* U+2665 */

#define RADIO_UI_REFRESH_MS     500
#define RADIO_UI_IDLE_MS        60000           /* back to the sleep screen without a touch */
#define RADIO_UI_VOLUME_STEP    10
#define RADIO_UI_TREBLE_STEP    2
#define RADIO_UI_TEXT           0xFFFFFF
#define RADIO_UI_SOFT           0xB9CCF5        /* as the clock and the costs */
#define RADIO_UI_HEART_ON       0xFF5A6E
#define RADIO_UI_CURRENT_BG     0x2E3A66
#define RADIO_UI_ROW_H          28
/* Buttons in the robot's colours: white turning pale blue, a thin blue edge and a soft blue glow */
#define RADIO_UI_BTN_TOP        0xFFFFFF
#define RADIO_UI_BTN_BOTTOM     0xC9DAFF
#define RADIO_UI_BTN_EDGE       0x4F86FF
#define RADIO_UI_BTN_SYMBOL     0x26305A
#define RADIO_UI_BTN_W          30
#define RADIO_UI_BTN_H          24
#define RADIO_UI_PLAY_W         44
#define RADIO_UI_MARGIN         12

static const char *TAG = "ui_radio";

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_title;
static lv_obj_t *s_list;
static lv_obj_t *s_empty;
static lv_obj_t *s_station;
static lv_obj_t *s_heart;
static lv_obj_t *s_volume;
static lv_obj_t *s_treble_label;
static lv_obj_t *s_play;

static radio_station_t *s_favorites = NULL;     /* as shown in the list (PSRAM) */
static int s_favorite_count = -1;
static char s_shown_url[RADIO_URL_SIZE];        /* the current station highlighted in the list */
static char s_shown_station[RADIO_NAME_SIZE + 140];

/*
 * ---------- the sleeping robot listening to the radio ----------
 * While a radio plays, the robot of the sleep screen wears headphones (a headband over its head and two ear cups,
 * children of the head: they bounce with it), its eyes smile ("^ ^", instead of half closed), and notes take the
 * place of its "z z" (the same fading animation: the notes are the background symbols of their containers).
 * The head is a disc of ~50 px radius in the middle of its 119 x 119 image.
 */
#define MUSIC_PHONES_COLOR      0x2B3350        /* dark blue, as the screen of the eyes */
#define MUSIC_PHONES_EDGE       0x4F86FF        /* the robot's blue glow */
#define MUSIC_EYES_COLOR        0xF8FCF8        /* as its eyes */
#define MUSIC_NOTES_COLOR       0xDDE9FF
#define MUSIC_NOTE_BIG          "\xE2\x99\xAB"  /* U+266B */
#define MUSIC_NOTE_SMALL        "\xE2\x99\xAA"  /* U+266A */

static lv_obj_t *s_phones[3];       /* headband, left and right ear cups */
static lv_obj_t *s_happy_eyes[2];
static const void *s_z_big = NULL;
static const void *s_z_small = NULL;
static bool s_music = false;

static lv_obj_t *ear_cup(lv_align_t align, lv_coord_t x)
{
    lv_obj_t *cup = lv_obj_create(ui_ImageSleepBody);
    lv_obj_remove_style_all(cup);
    lv_obj_set_size(cup, 15, 36);
    lv_obj_set_style_radius(cup, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(cup, lv_color_hex(MUSIC_PHONES_COLOR), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(cup, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(cup, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(cup, lv_color_hex(MUSIC_PHONES_EDGE), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(cup, align, x, 2);
    lv_obj_clear_flag(cup, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return cup;
}

static lv_obj_t *band_arc(lv_obj_t *parent, lv_coord_t size, lv_coord_t width, uint16_t start, uint16_t end,
                          uint32_t color)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, size, size);
    lv_arc_set_bg_angles(arc, start, end);
    lv_arc_set_angles(arc, start, end);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    return arc;
}

static void music_robot_create(void)
{
    // Headband: the top of the head, slightly outside it; a blue line along it
    s_phones[0] = band_arc(ui_ImageSleepBody, 114, 7, 195, 345, MUSIC_PHONES_COLOR);
    lv_obj_align(s_phones[0], LV_ALIGN_CENTER, -1, 2);
    lv_obj_t *line = band_arc(s_phones[0], 114, 2, 197, 343, MUSIC_PHONES_EDGE);
    lv_obj_center(line);
    s_phones[1] = ear_cup(LV_ALIGN_LEFT_MID, 2);
    s_phones[2] = ear_cup(LV_ALIGN_RIGHT_MID, -4);
    // Smiling eyes, in the screen of the eyes
    for (int i = 0; i < 2; i++) {
        s_happy_eyes[i] = band_arc(ui_Image3, 14, 3, 200, 340, MUSIC_EYES_COLOR);
        lv_obj_align(s_happy_eyes[i], LV_ALIGN_CENTER, i ? 9 : -9, 4);
    }
    s_z_big = lv_obj_get_style_bg_img_src(ui_ContainerBigZ, LV_PART_MAIN);
    s_z_small = lv_obj_get_style_bg_img_src(ui_ContainerSmallZ, LV_PART_MAIN);
    for (int i = 0; i < 3; i++) {
        lv_obj_add_flag(s_phones[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 2; i++) {
        lv_obj_add_flag(s_happy_eyes[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void z_or_note(lv_obj_t *z, const void *image, const char *note, bool music)
{
    lv_obj_set_style_bg_img_src(z, music ? (const void *)note : image, LV_PART_MAIN | LV_STATE_DEFAULT);
    if (music) {
        // A symbol as background image is drawn with the text font and the text colour of its object
        lv_obj_set_style_text_font(z, &ui_font_Note22, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(z, lv_color_hex(MUSIC_NOTES_COLOR), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_size(z, 28, 28);     // the note is taller than the z
    }
}

static void music_robot_show(bool music)
{
    if (music == s_music || !s_phones[0]) {
        return;
    }
    s_music = music;
    for (int i = 0; i < 3; i++) {
        music ? lv_obj_clear_flag(s_phones[i], LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(s_phones[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 2; i++) {
        music ? lv_obj_clear_flag(s_happy_eyes[i], LV_OBJ_FLAG_HIDDEN) :
                lv_obj_add_flag(s_happy_eyes[i], LV_OBJ_FLAG_HIDDEN);
    }
    music ? lv_obj_add_flag(ui_ImageSleepEye, LV_OBJ_FLAG_HIDDEN) : lv_obj_clear_flag(ui_ImageSleepEye, LV_OBJ_FLAG_HIDDEN);
    z_or_note(ui_ContainerBigZ, s_z_big, MUSIC_NOTE_BIG, music);
    z_or_note(ui_ContainerSmallZ, s_z_small, MUSIC_NOTE_SMALL, music);
    if (!music) {
        lv_obj_set_size(ui_ContainerBigZ, 27, 22);      // their sizes of the generated screen
        lv_obj_set_size(ui_ContainerSmallZ, 23, 17);
    }
    ESP_LOGI(TAG, "Sleeping robot %s", music ? "listening to the radio" : "asleep");
}

/* ---------- actions ---------- */

static void back_cb(lv_event_t *e)
{
    lv_scr_load(ui_ScreenListen);
}

static void favorite_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < s_favorite_count) {
        radio_play_list(s_favorites, s_favorite_count, index);
    }
}

static void play_cb(lv_event_t *e)
{
    if (radio_state() == RADIO_PLAYING) {
        radio_pause();
    } else if (!radio_resume() && s_favorite_count > 0) {
        radio_play_list(s_favorites, s_favorite_count, 0);     // no station yet: the first favourite
    }
}

/* < and >: the previous / next station of the list played (a search or the favourites) */
static void move_cb(lv_event_t *e)
{
    if ((intptr_t)lv_event_get_user_data(e) > 0) {
        radio_next();
    } else {
        radio_previous();
    }
}

static void heart_cb(lv_event_t *e)
{
    radio_station_t station = radio_station();
    if (!station.url[0]) {
        return;
    }
    if (radio_favorites_contains(&station)) {
        radio_favorites_remove(&station);
    } else {
        radio_favorites_add(&station);
    }
    s_favorite_count = -1;      // the list is made again at the next refresh
}

static void volume_cb(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    app_volume_request(app_volume_get() + delta);   // applied now (no speech): saved when the assistant sleeps
}

static void treble_cb(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    sys_param_t *param = settings_get_parameter();
    int db = param->radio_treble + delta;
    db = db < -12 ? -12 : (db > 6 ? 6 : db);
    if (db == param->radio_treble) {
        return;
    }
    param->radio_treble = db;
    radio_set_treble(db);
    char value[8];
    snprintf(value, sizeof(value), "%d", db);
    settings_save_str("Radio_treble", value);
}

/* ---------- screen ---------- */

static lv_obj_t *glow_button(lv_obj_t *parent, const char *symbol, lv_coord_t w, lv_event_cb_t cb, intptr_t data,
                             lv_obj_t **label_out)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, RADIO_UI_BTN_H);
    lv_obj_set_style_radius(btn, RADIO_UI_BTN_H / 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(RADIO_UI_BTN_TOP), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_color(btn, lv_color_hex(RADIO_UI_BTN_BOTTOM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn, lv_color_hex(RADIO_UI_BTN_EDGE), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(btn, LV_OPA_70, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(RADIO_UI_BTN_EDGE), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_50, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_color(btn, lv_color_hex(RADIO_UI_BTN_EDGE), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(btn, 6);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)data);
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, lv_color_hex(RADIO_UI_BTN_SYMBOL), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(label, symbol);
    lv_obj_center(label);
    if (label_out) {
        *label_out = label;
    }
    return btn;
}

static lv_obj_t *text_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(label, "");
    return label;
}

static void screen_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_img_src(s_screen, &ui_img_setup_bg_png, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *back = lv_img_create(s_screen);
    lv_img_set_src(back, &ui_img_settings_back_png);
    lv_obj_set_pos(back, 10, 10);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_set_ext_click_area(back, 10);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

    s_title = text_label(s_screen, &ui_font_Latin20, RADIO_UI_TEXT);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 12);

    // The favourites: a row each, scrolled with the finger
    s_list = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, 320 - 2 * RADIO_UI_MARGIN, 3 * RADIO_UI_ROW_H + 8);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
    s_empty = text_label(s_screen, &ui_font_Latin14, RADIO_UI_SOFT);
    lv_label_set_long_mode(s_empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_empty, 320 - 2 * RADIO_UI_MARGIN);
    lv_obj_align(s_empty, LV_ALIGN_TOP_MID, 0, 50);

    // The station playing and its heart
    s_station = text_label(s_screen, &ui_font_Latin16, RADIO_UI_TEXT);
    lv_label_set_long_mode(s_station, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_station, 320 - 2 * RADIO_UI_MARGIN - 40);
    lv_obj_align(s_station, LV_ALIGN_TOP_LEFT, RADIO_UI_MARGIN, 140);
    s_heart = text_label(s_screen, &ui_font_Radio28, RADIO_UI_SOFT);
    lv_obj_align(s_heart, LV_ALIGN_TOP_RIGHT, -RADIO_UI_MARGIN, 132);
    lv_obj_add_flag(s_heart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_heart, 12);
    lv_obj_add_event_cb(s_heart, heart_cb, LV_EVENT_CLICKED, NULL);

    // Bottom left: the volume and its [-] [+]; bottom right: the treble and its [-] [+]; play / pause between
    const lv_coord_t buttons_y = -10;
    const lv_coord_t labels_y = buttons_y - RADIO_UI_BTN_H - 8;
    lv_obj_t *volume_minus = glow_button(s_screen, LV_SYMBOL_MINUS, RADIO_UI_BTN_W, volume_cb, -RADIO_UI_VOLUME_STEP,
                                         NULL);
    lv_obj_align(volume_minus, LV_ALIGN_BOTTOM_LEFT, RADIO_UI_MARGIN, buttons_y);
    lv_obj_t *volume_plus = glow_button(s_screen, LV_SYMBOL_PLUS, RADIO_UI_BTN_W, volume_cb, RADIO_UI_VOLUME_STEP,
                                        NULL);
    lv_obj_align_to(volume_plus, volume_minus, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    s_volume = text_label(s_screen, &ui_font_Latin14, RADIO_UI_SOFT);
    lv_obj_align(s_volume, LV_ALIGN_BOTTOM_LEFT, RADIO_UI_MARGIN, labels_y);

    lv_obj_t *treble_plus = glow_button(s_screen, LV_SYMBOL_PLUS, RADIO_UI_BTN_W, treble_cb, RADIO_UI_TREBLE_STEP,
                                        NULL);
    lv_obj_align(treble_plus, LV_ALIGN_BOTTOM_RIGHT, -RADIO_UI_MARGIN, buttons_y);
    lv_obj_t *treble_minus = glow_button(s_screen, LV_SYMBOL_MINUS, RADIO_UI_BTN_W, treble_cb, -RADIO_UI_TREBLE_STEP,
                                         NULL);
    lv_obj_align_to(treble_minus, treble_plus, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    s_treble_label = text_label(s_screen, &ui_font_Latin14, RADIO_UI_SOFT);
    lv_obj_align(s_treble_label, LV_ALIGN_BOTTOM_RIGHT, -RADIO_UI_MARGIN, labels_y);

    lv_obj_t *play = glow_button(s_screen, LV_SYMBOL_PLAY, RADIO_UI_PLAY_W, play_cb, 0, &s_play);
    lv_obj_align(play, LV_ALIGN_BOTTOM_MID, 0, buttons_y);
    lv_obj_t *previous = glow_button(s_screen, LV_SYMBOL_PREV, RADIO_UI_BTN_W, move_cb, -1, NULL);
    lv_obj_align_to(previous, play, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    lv_obj_t *next = glow_button(s_screen, LV_SYMBOL_NEXT, RADIO_UI_BTN_W, move_cb, 1, NULL);
    lv_obj_align_to(next, play, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
}

/* The favourites' rows made again (the file read when the screen opens, or after a heart) */
static void list_fill(void)
{
    lv_obj_clean(s_list);
    s_favorite_count = radio_favorites_list(s_favorites, RADIO_LIST_MAX);
    s_shown_url[0] = '\0';
    for (int i = 0; i < s_favorite_count; i++) {
        lv_obj_t *row = lv_btn_create(s_list);
        lv_obj_set_size(row, lv_pct(100), RADIO_UI_ROW_H);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(row, lv_color_hex(RADIO_UI_CURRENT_BG), LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(row, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(row, favorite_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *label = text_label(row, &ui_font_Latin16, RADIO_UI_TEXT);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_text(label, s_favorites[i].name);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    }
    lv_label_set_text(s_empty, s_favorite_count ? "" : app_lang_get()->radio_fav_empty);
}

static void screen_refresh(void)
{
    const app_lang_t *lang = app_lang_get();
    if (s_favorite_count < 0) {
        list_fill();
    }
    radio_station_t station = radio_station();
    // The current station highlighted in the list
    if (strcmp(station.url, s_shown_url) != 0) {
        strlcpy(s_shown_url, station.url, sizeof(s_shown_url));
        for (int i = 0; i < s_favorite_count; i++) {
            lv_obj_t *row = lv_obj_get_child(s_list, i);
            if (strcmp(s_favorites[i].url, station.url) == 0) {
                lv_obj_add_state(row, LV_STATE_CHECKED);
                lv_obj_scroll_to_view(row, LV_ANIM_OFF);
            } else {
                lv_obj_clear_state(row, LV_STATE_CHECKED);
            }
        }
    }
    // "Classic 21 - Pink Floyd, Time": set only when it changes (else its scrolling starts again)
    char text[sizeof(s_shown_station)];
    char title[128];
    radio_title(title, sizeof(title));
    if (!station.url[0]) {
        strlcpy(text, lang->radio_nothing, sizeof(text));
    } else if (title[0]) {
        snprintf(text, sizeof(text), "%s - %s", station.name, title);
    } else {
        strlcpy(text, station.name, sizeof(text));
    }
    if (strcmp(text, s_shown_station) != 0) {
        strlcpy(s_shown_station, text, sizeof(s_shown_station));
        lv_label_set_text(s_station, text);
    }
    bool favorite = station.url[0] && radio_favorites_contains(&station);
    lv_label_set_text(s_heart, station.url[0] ? (favorite ? SYM_HEART_FULL : SYM_HEART_EMPTY) : "");
    lv_obj_set_style_text_color(s_heart, lv_color_hex(favorite ? RADIO_UI_HEART_ON : RADIO_UI_SOFT),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(s_play, radio_state() == RADIO_PLAYING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_label_set_text_fmt(s_volume, lang->radio_volume_label, app_volume_get());
    char treble[48];
    snprintf(treble, sizeof(treble), "%s %+d dB", lang->radio_treble_label, settings_get_parameter()->radio_treble);
    lv_label_set_text(s_treble_label, treble);
}

static void refresh_cb(lv_timer_t *timer)
{
    // Suspended by the conversation, the radio plays again afterwards: the robot keeps its headphones (no sleeping
    // robot shown for an instant between the wake word and the listening screen)
    music_robot_show(radio_state() == RADIO_PLAYING || radio_state() == RADIO_SUSPENDED);
    if (!s_screen || lv_scr_act() != s_screen) {
        return;
    }
    if (lv_disp_get_inactive_time(NULL) > RADIO_UI_IDLE_MS) {
        lv_scr_load(ui_ScreenListen);
        return;
    }
    screen_refresh();
}

static void open_cb(lv_event_t *e)
{
    if (!s_screen) {
        screen_create();
    }
    lv_label_set_text(s_title, app_lang_get()->radio_title);
    s_favorite_count = -1;      // the file may have changed (configuration drive)
    s_shown_station[0] = '\0';
    screen_refresh();
    lv_scr_load(s_screen);
    ESP_LOGI(TAG, "Radio screen");
}

void ui_radio_init(void)
{
    s_favorites = heap_caps_calloc(RADIO_LIST_MAX, sizeof(radio_station_t), MALLOC_CAP_SPIRAM);
    // The volume of the sleep screen (bottom left) opens the radio screen: a clickable zone over it, which takes the
    // tap instead of the sleep panel (whose tap wakes the assistant up)
    lv_obj_t *zone = lv_obj_create(ui_PanelSleep);
    lv_obj_remove_style_all(zone);
    lv_obj_set_size(zone, 70, 140);
    lv_obj_align(zone, LV_ALIGN_BOTTOM_LEFT, 6, 0);
    lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(zone, open_cb, LV_EVENT_CLICKED, NULL);
    music_robot_create();
    lv_timer_create(refresh_cb, RADIO_UI_REFRESH_MS, NULL);
}

void ui_radio_close(void)
{
    if (s_screen && lv_scr_act() == s_screen) {
        lv_scr_load(ui_ScreenListen);
    }
}
