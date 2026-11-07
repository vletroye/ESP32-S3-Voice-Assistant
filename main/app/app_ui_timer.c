/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Countdown of a timer and the stopwatch eyes: see app_ui_timer.h.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "ui.h"
#include "alarms.h"
#include "app_ui_timer.h"

LV_FONT_DECLARE(ui_font_Latin20);

#define TIMER_REFRESH_MS    200
/* Colours of the clock: pale blue-white with a blue glow (app_clock.c) */
#define COLOR_DIGIT         0xDDE9FF
#define COLOR_GLOW          0x4F86FF
#define COLOR_HAND          0xFFB300

/* The countdown at the top middle, level with the costs at the top right */
#define COUNT_TOP_Y         10

/* A stopwatch per eye in the screen of the eyes (EYES_W x EYES_H): a dial, its button on top, two hands */
#define EYES_W              60
#define EYES_H              34
#define WATCH_W             26
#define WATCH_H             EYES_H
#define WATCH_R             11          /* the dial's radius */
#define WATCH_BUTTON_W      6
#define WATCH_BUTTON_H      3
/* The long hand turns once in this time, the short one 12 times slower */
#define WATCH_TURN_MS       2000

static lv_obj_t *s_count = NULL;
static lv_obj_t *s_watches[2];
static int32_t s_angle = 0;             /* the long hand, in tenths of a degree */
static bool s_watches_shown = false;

/* A hand from the centre, at this angle (tenths of a degree, 0 at the top, clockwise) */
static void hand_draw(lv_draw_ctx_t *ctx, const lv_point_t *centre, int32_t angle, lv_coord_t length,
                      lv_coord_t width)
{
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_hex(COLOR_HAND);
    line.width = width;
    line.round_start = 1;
    line.round_end = 1;
    float a = (float)angle * (float)M_PI / 1800.0f;
    lv_point_t end = {
        .x = centre->x + (lv_coord_t)lroundf(sinf(a) * length),
        .y = centre->y - (lv_coord_t)lroundf(cosf(a) * length),
    };
    lv_draw_line(ctx, &line, centre, &end);
}

static void watch_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t box;
    lv_obj_get_coords(obj, &box);
    lv_point_t centre = { .x = box.x1 + WATCH_W / 2, .y = box.y2 - WATCH_R - 1 };

    // The button on top, and its stem
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_color = lv_color_hex(COLOR_DIGIT);
    rect.radius = 1;
    lv_area_t button = {
        .x1 = centre.x - WATCH_BUTTON_W / 2, .x2 = centre.x + WATCH_BUTTON_W / 2,
        .y1 = centre.y - WATCH_R - WATCH_BUTTON_H - 2, .y2 = centre.y - WATCH_R - 2,
    };
    lv_draw_rect(ctx, &rect, &button);

    // The dial: a glow, then the ring
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = lv_color_hex(COLOR_GLOW);
    arc.opa = LV_OPA_40;
    arc.width = 4;
    lv_draw_arc(ctx, &arc, &centre, WATCH_R + 1, 0, 360);
    arc.color = lv_color_hex(COLOR_DIGIT);
    arc.opa = LV_OPA_COVER;
    arc.width = 2;
    lv_draw_arc(ctx, &arc, &centre, WATCH_R, 0, 360);

    // The hands, then the pin
    hand_draw(ctx, &centre, s_angle / 12, WATCH_R - 6, 2);
    hand_draw(ctx, &centre, s_angle, WATCH_R - 3, 1);
    rect.bg_color = lv_color_hex(COLOR_HAND);
    rect.radius = LV_RADIUS_CIRCLE;
    lv_area_t pin = { .x1 = centre.x - 1, .x2 = centre.x + 1, .y1 = centre.y - 1, .y2 = centre.y + 1 };
    lv_draw_rect(ctx, &rect, &pin);
}

static void turn_anim_cb(void *var, int32_t v)
{
    s_angle = v;
    if (s_watches_shown) {
        lv_obj_invalidate(s_watches[0]);
        lv_obj_invalidate(s_watches[1]);
    }
}

/* Asleep with a timer: the stopwatches instead of the closed eyes and the Z */
static void watches_show(bool shown)
{
    if (shown == s_watches_shown) {
        return;
    }
    s_watches_shown = shown;
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
            lv_obj_clear_flag(s_watches[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_watches[i], LV_OBJ_FLAG_HIDDEN);
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
    if (strcmp(lv_label_get_text(s_count), text) != 0) {
        lv_label_set_text(s_count, text);
    }
    // Shown asleep, listening ("J'écoute"), heard and thinking ("Entendu", "Je réfléchis"), not over a reply
    bool robot_shown = !lv_obj_has_flag(ui_PanelSleep, LV_OBJ_FLAG_HIDDEN) ||
                       !lv_obj_has_flag(ui_PanelListen, LV_OBJ_FLAG_HIDDEN) ||
                       !lv_obj_has_flag(ui_PanelGet, LV_OBJ_FLAG_HIDDEN);
    if (text[0] && robot_shown && lv_obj_has_flag(ui_PanelReply, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(s_count, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_count, LV_OBJ_FLAG_HIDDEN);
    }
    watches_show(left >= 0);
}

void ui_timer_init(void)
{
    s_count = lv_label_create(ui_ScreenListen);
    lv_obj_set_style_text_font(s_count, &ui_font_Latin20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_count, lv_color_hex(COLOR_DIGIT), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(s_count, 1, LV_PART_MAIN);
    lv_label_set_text(s_count, "");
    lv_obj_align(s_count, LV_ALIGN_TOP_MID, 0, COUNT_TOP_Y);
    lv_obj_clear_flag(s_count, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_count, LV_OBJ_FLAG_HIDDEN);

    // The stopwatches in the screen of the eyes asleep, one per eye
    for (int i = 0; i < 2; i++) {
        s_watches[i] = lv_obj_create(ui_Image3);
        lv_obj_remove_style_all(s_watches[i]);
        lv_obj_clear_flag(s_watches[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(s_watches[i], WATCH_W, WATCH_H);
        lv_obj_set_pos(s_watches[i], i == 0 ? (EYES_W / 2 - WATCH_W) / 2 : EYES_W / 2 + (EYES_W / 2 - WATCH_W) / 2, 0);
        lv_obj_add_event_cb(s_watches[i], watch_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
        lv_obj_add_flag(s_watches[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_angle);
    lv_anim_set_exec_cb(&a, turn_anim_cb);
    lv_anim_set_values(&a, 0, 3600 * 12 - 1);   // 12 turns of the long hand: one of the short one
    lv_anim_set_time(&a, WATCH_TURN_MS * 12);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    lv_timer_create(refresh_cb, TIMER_REFRESH_MS, NULL);
}
