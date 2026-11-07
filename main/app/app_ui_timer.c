/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Countdown of a timer and the split-flap eyes: see app_ui_timer.h.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "ui.h"
#include "alarms.h"
#include "app_ui_timer.h"

LV_FONT_DECLARE(ui_font_Latin20);

#define TIMER_REFRESH_MS    200
/* Colours of the clock: pale blue-white digits with a blue glow (app_clock.c) */
#define COLOR_DIGIT         0xDDE9FF
#define COLOR_GLOW          0x4F86FF
#define COLOR_FLAP          0x1B2240
#define COLOR_HINGE         0x05070F

/* A flap in the screen of the eyes (EYES_W x EYES_H): two of them, side by side, centred */
#define EYES_W              60
#define EYES_H              34
#define FLAP_W              24
#define FLAP_H              28
#define FLAP_HALF           (FLAP_H / 2)
#define FLAP_GAP            4
#define FLAP_FLIP_MS        280

typedef struct {
    lv_obj_t *box;
    lv_obj_t *top, *top_label;          /* the upper half: the new digit, uncovered as the flap falls */
    lv_obj_t *bottom, *bottom_label;    /* the lower half: the old digit until the flap lands */
    lv_obj_t *flap, *flap_label;        /* the half turning over: the old upper half, then the new lower half */
    char digit[2];
    char old[2];
} flap_t;

static flap_t s_flaps[2];
static lv_obj_t *s_counts[3];           /* above the heads asleep, listening, thinking */
static bool s_flaps_shown = false;

/* A half of a flap: a box clipping a label of the flap's full height */
static lv_obj_t *half_create(lv_obj_t *parent, lv_coord_t y, lv_obj_t **label)
{
    lv_obj_t *half = lv_obj_create(parent);
    lv_obj_remove_style_all(half);
    lv_obj_clear_flag(half, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(half, lv_color_hex(COLOR_FLAP), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(half, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_size(half, FLAP_W, FLAP_HALF);
    lv_obj_set_pos(half, 0, y);
    *label = lv_label_create(half);
    lv_obj_set_size(*label, FLAP_W, FLAP_H);
    lv_obj_set_style_text_font(*label, &ui_font_Latin20, LV_PART_MAIN);
    lv_obj_set_style_text_color(*label, lv_color_hex(COLOR_DIGIT), LV_PART_MAIN);
    lv_obj_set_style_text_align(*label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_pad_top(*label, (FLAP_H - lv_font_get_line_height(&ui_font_Latin20)) / 2, LV_PART_MAIN);
    lv_label_set_text(*label, "0");
    lv_obj_set_pos(*label, 0, -y);
    return half;
}

static void flap_create(flap_t *f, lv_obj_t *parent, lv_coord_t x, lv_coord_t y)
{
    f->box = lv_obj_create(parent);
    lv_obj_remove_style_all(f->box);
    lv_obj_clear_flag(f->box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(f->box, FLAP_W, FLAP_H);
    lv_obj_set_pos(f->box, x, y);
    lv_obj_set_style_radius(f->box, 3, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(f->box, true, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(f->box, lv_color_hex(COLOR_GLOW), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(f->box, 6, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(f->box, LV_OPA_40, LV_PART_MAIN);
    f->top = half_create(f->box, 0, &f->top_label);
    f->bottom = half_create(f->box, FLAP_HALF, &f->bottom_label);
    f->flap = half_create(f->box, 0, &f->flap_label);
    lv_obj_add_flag(f->flap, LV_OBJ_FLAG_HIDDEN);
    // The hinge: a dark line across the middle
    lv_obj_t *hinge = lv_obj_create(f->box);
    lv_obj_remove_style_all(hinge);
    lv_obj_set_size(hinge, FLAP_W, 1);
    lv_obj_set_pos(hinge, 0, FLAP_HALF);
    lv_obj_set_style_bg_color(hinge, lv_color_hex(COLOR_HINGE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hinge, LV_OPA_COVER, LV_PART_MAIN);
    strcpy(f->digit, "0");
    strcpy(f->old, "0");
}

/*
 * The flap turning over, v from 0 to FLAP_H: first the old upper half falls towards the hinge (shrinking), then the
 * new lower half unfolds from it (growing), covering the old one
 */
static void flip_anim_cb(void *var, int32_t v)
{
    flap_t *f = var;
    if (v < FLAP_HALF) {
        lv_coord_t h = FLAP_HALF - v;
        lv_label_set_text(f->flap_label, f->old);
        lv_obj_set_height(f->flap, h);
        lv_obj_set_y(f->flap, FLAP_HALF - h);
        lv_obj_set_y(f->flap_label, h - FLAP_HALF);
    } else {
        lv_coord_t h = v - FLAP_HALF;
        lv_label_set_text(f->flap_label, f->digit);
        lv_obj_set_height(f->flap, h);
        lv_obj_set_y(f->flap, FLAP_HALF);
        lv_obj_set_y(f->flap_label, -FLAP_HALF);
    }
}

static void flip_ready_cb(lv_anim_t *a)
{
    flap_t *f = a->var;
    lv_label_set_text(f->bottom_label, f->digit);
    lv_obj_add_flag(f->flap, LV_OBJ_FLAG_HIDDEN);
}

/* The flap shows this digit: turned over when it changes (at once when not shown) */
static void flap_set(flap_t *f, char digit, bool animate)
{
    if (f->digit[0] == digit) {
        return;
    }
    f->old[0] = f->digit[0];
    f->digit[0] = digit;
    lv_label_set_text(f->top_label, f->digit);
    if (!animate) {
        lv_label_set_text(f->bottom_label, f->digit);
        return;
    }
    lv_label_set_text(f->bottom_label, f->old);
    lv_obj_clear_flag(f->flap, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, f);
    lv_anim_set_exec_cb(&a, flip_anim_cb);
    lv_anim_set_values(&a, 0, FLAP_H);
    lv_anim_set_time(&a, FLAP_FLIP_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, flip_ready_cb);
    lv_anim_start(&a);
}

/* Asleep with a timer: the flaps instead of the closed eyes and the Z */
static void flaps_show(bool shown)
{
    if (shown == s_flaps_shown) {
        return;
    }
    s_flaps_shown = shown;
    lv_obj_t *sleep[] = { ui_ImageSleepEye, ui_ContainerBigZ, ui_ContainerSmallZ };
    for (size_t i = 0; i < sizeof(sleep) / sizeof(sleep[0]); i++) {
        if (shown) {
            lv_obj_add_flag(sleep[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(sleep[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < 2; i++) {
        if (shown) {
            lv_obj_clear_flag(s_flaps[i].box, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_flaps[i].box, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void refresh_cb(lv_timer_t *t)
{
    alarm_t timer;
    long left = -1;
    if (alarms_next(ALARM_KIND_TIMER, &timer)) {
        left = (long)(timer.due - time(NULL));
        left = left < 0 ? 0 : left;
    }
    char text[16] = "";
    if (left >= 3600) {
        snprintf(text, sizeof(text), "%ld:%02ld:%02ld", left / 3600, left / 60 % 60, left % 60);
    } else if (left >= 0) {
        snprintf(text, sizeof(text), "%ld:%02ld", left / 60, left % 60);
    }
    for (int i = 0; i < 3; i++) {
        if (strcmp(lv_label_get_text(s_counts[i]), text) != 0) {
            lv_label_set_text(s_counts[i], text);
        }
        if (text[0]) {
            lv_obj_clear_flag(s_counts[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_counts[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    bool was_shown = s_flaps_shown;
    flaps_show(left >= 0);
    if (left >= 0) {
        int seconds = (int)(left % 60);
        flap_set(&s_flaps[0], (char)('0' + seconds / 10), was_shown);
        flap_set(&s_flaps[1], (char)('0' + seconds % 10), was_shown);
    }
}

void ui_timer_init(lv_coord_t above_y)
{
    lv_obj_t *eye_screens[3] = { ui_Image3, ui_ImageListenEyeScreen, ui_ImageGetEyeScreen };
    for (int i = 0; i < 3; i++) {
        s_counts[i] = lv_label_create(eye_screens[i]);
        lv_obj_set_style_text_font(s_counts[i], &ui_font_Latin20, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_counts[i], lv_color_hex(COLOR_DIGIT), LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(s_counts[i], 1, LV_PART_MAIN);
        lv_label_set_text(s_counts[i], "");
        lv_obj_align(s_counts[i], LV_ALIGN_OUT_TOP_MID, 0, -above_y);
        lv_obj_clear_flag(s_counts[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_counts[i], LV_OBJ_FLAG_HIDDEN);
    }
    // The flaps in the screen of the eyes asleep
    const lv_coord_t x = (EYES_W - 2 * FLAP_W - FLAP_GAP) / 2;
    const lv_coord_t y = (EYES_H - FLAP_H) / 2;
    for (int i = 0; i < 2; i++) {
        flap_create(&s_flaps[i], ui_Image3, x + i * (FLAP_W + FLAP_GAP), y);
        lv_obj_add_flag(s_flaps[i].box, LV_OBJ_FLAG_HIDDEN);
    }
    lv_timer_create(refresh_cb, TIMER_REFRESH_MS, NULL);
}
