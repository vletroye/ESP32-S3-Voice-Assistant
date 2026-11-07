/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Screen of the timers, alarms and reminders: see app_ui_alarms.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "ui.h"
#include "file_lang.h"
#include "alarms.h"
#include "app_ui_alarms.h"

LV_FONT_DECLARE(ui_font_Latin14);
LV_FONT_DECLARE(ui_font_Latin20);

#define ALARMS_REFRESH_MS   1000
#define ALARMS_DIM          0x8A97BC
/* The zone over the clock and the date of the sleep screen (app_clock.c) that opens this screen */
#define CLOCK_ZONE_W        160
#define CLOCK_ZONE_H        64

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_table = NULL;

/* A row: when on the left, what on the right (two lines); a section's title alone, in white */
static void row(int *r, const char *when, const char *what)
{
    lv_table_set_cell_value(s_table, *r, 0, when);
    lv_table_set_cell_value(s_table, *r, 1, what ? what : "");
    (*r)++;
}

/* "1:05:30", "4:12" */
static void left_text(long seconds, char *out, size_t size)
{
    seconds = seconds < 0 ? 0 : seconds;
    if (seconds >= 3600) {
        snprintf(out, size, "%ld:%02ld:%02ld", seconds / 3600, seconds / 60 % 60, seconds % 60);
    } else {
        snprintf(out, size, "%ld:%02ld", seconds / 60, seconds % 60);
    }
}

/* How it repeats, in words ("en semaine", "chaque lundi, jeudi", "le 2e mardi du mois") */
static void repeat_text(const alarm_t *a, char *out, size_t size)
{
    const app_lang_t *lang = app_lang_get();
    struct tm t;
    localtime_r(&a->due, &t);
    switch (a->repeat) {
    case ALARM_REPEAT_DAILY:
        strlcpy(out, lang->repeat_daily, size);
        break;
    case ALARM_REPEAT_WEEKDAYS:
        strlcpy(out, lang->repeat_weekdays, size);
        break;
    case ALARM_REPEAT_WEEKEND:
        strlcpy(out, lang->repeat_weekend, size);
        break;
    case ALARM_REPEAT_WEEKLY: {
        char days[96] = "";
        for (int i = 1; i <= 7; i++) {      // Monday first
            int d = i % 7;
            if ((a->days >> d) & 1) {
                if (days[0]) {
                    strlcat(days, ", ", sizeof(days));
                }
                strlcat(days, lang->weekdays_long[d], sizeof(days));
            }
        }
        snprintf(out, size, lang->repeat_weekly, days);
        break;
    }
    case ALARM_REPEAT_MONTHLY:
        snprintf(out, size, lang->repeat_monthly, a->month_day);
        break;
    case ALARM_REPEAT_MONTHLY_NTH: {
        // "1er, 2e, 3e, 4e, 5e": the nth of them
        char ordinal[16] = "";
        const char *p = lang->repeat_ordinals;
        for (int n = 1; n < a->nth && p && *p; n++) {
            p = strchr(p, ',');
            p = p ? p + 1 + (p[1] == ' ') : NULL;
        }
        if (p) {
            size_t len = strcspn(p, ",");
            snprintf(ordinal, sizeof(ordinal), "%.*s", (int)len, p);
        }
        snprintf(out, size, lang->repeat_monthly_nth, ordinal, lang->weekdays_long[a->week_day]);
        break;
    }
    case ALARM_REPEAT_MONTHLY_LAST:
        snprintf(out, size, lang->repeat_monthly_last, lang->weekdays_long[a->week_day]);
        break;
    case ALARM_REPEAT_MONTHLY_END:
        strlcpy(out, lang->repeat_monthly_end, size);
        break;
    case ALARM_REPEAT_YEARLY:
        strlcpy(out, lang->repeat_yearly, size);
        break;
    default:
        strlcpy(out, lang->repeat_once, size);
        break;
    }
}

static void fill(void)
{
    const app_lang_t *lang = app_lang_get();
    lv_label_set_text(s_title, lang->alarms_title);
    int count = 0;
    alarm_t *list = alarms_list(&count);
    time_t now = time(NULL);
    int r = 0;
    static const alarm_kind_t kinds[] = { ALARM_KIND_TIMER, ALARM_KIND_ALARM, ALARM_KIND_REMINDER };
    const char *sections[] = { lang->alarms_timers, lang->alarms_alarms, lang->alarms_reminders };
    for (size_t k = 0; k < 3; k++) {
        bool titled = false;
        for (int i = 0; i < count; i++) {
            const alarm_t *a = &list[i];
            if (a->kind != kinds[k]) {
                continue;
            }
            if (!titled) {
                row(&r, sections[k], "");
                titled = true;
            }
            struct tm t;
            localtime_r(&a->due, &t);
            char when[32], what[ALARM_LABEL_SIZE + ALARM_RADIO_SIZE + 96], detail[96], left[16];
            const char *label = a->label[0] ? a->label : "-";
            if (a->kind == ALARM_KIND_TIMER) {
                strftime(when, sizeof(when), "%H:%M:%S", &t);
                left_text((long)(a->due - now), left, sizeof(left));
                snprintf(detail, sizeof(detail), lang->alarms_left, left);
            } else {
                // The day, then the time: "JEU 12/11\n07:30"
                snprintf(when, sizeof(when), "%s %02d/%02d\n%02d:%02d", lang->weekdays[t.tm_wday], t.tm_mday,
                         t.tm_mon + 1, t.tm_hour, t.tm_min);
                repeat_text(a, detail, sizeof(detail));
                if (a->radio[0]) {
                    size_t n = strlen(detail);
                    snprintf(detail + n, sizeof(detail) - n, lang->alarms_radio, a->radio);
                }
            }
            snprintf(what, sizeof(what), "%s\n#%06X %s#", label, ALARMS_DIM, detail);
            row(&r, when, what);
        }
    }
    free(list);
    if (r == 0) {
        row(&r, lang->alarms_none, "");
    }
    // The rows left over emptied first: LVGL 8 frees the cells of the rows it drops, and crashes on one never set
    for (uint16_t i = r; i < lv_table_get_row_cnt(s_table); i++) {
        lv_table_set_cell_value(s_table, i, 0, "");
        lv_table_set_cell_value(s_table, i, 1, "");
    }
    lv_table_set_row_cnt(s_table, r);
}

/* The rows naming a section in white, the others' times in the usual colour; the details dim (recoloured) */
static void table_draw_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->part != LV_PART_ITEMS || !dsc->label_dsc) {
        return;
    }
    dsc->label_dsc->flag |= LV_TEXT_FLAG_RECOLOR;
    uint32_t r = dsc->id / 2, c = dsc->id % 2;
    const char *value = lv_table_get_cell_value(s_table, r, 1);
    bool section = (value && !value[0]);
    if (c == 0 && section) {
        dsc->label_dsc->color = lv_color_hex(0xFFFFFF);
    }
}

static void refresh_cb(lv_timer_t *t)
{
    if (s_screen && lv_scr_act() == s_screen) {
        fill();
    }
}

static void back_cb(lv_event_t *e)
{
    lv_scr_load(ui_ScreenListen);
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
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

    s_title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_title, &ui_font_Latin20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 14);

    // The lines, scrolled with the finger when they do not fit
    s_table = lv_table_create(s_screen);
    lv_table_set_col_cnt(s_table, 2);
    lv_table_set_row_cnt(s_table, 1);
    lv_table_set_col_width(s_table, 0, 96);
    lv_table_set_col_width(s_table, 1, 208);
    lv_obj_set_style_text_font(s_table, &ui_font_Latin14, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_table, lv_color_hex(0xFFFFFF), LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_table, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_table, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_table, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_table, 1, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_table, lv_color_hex(0x5A6A9A), LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(s_table, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_table, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(s_table, 4, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(s_table, 4, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(s_table, 4, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(s_table, 4, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_size(s_table, 308, 240 - 48);
    lv_obj_align(s_table, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_scroll_dir(s_table, LV_DIR_VER);
    lv_obj_add_event_cb(s_table, table_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

    lv_timer_create(refresh_cb, ALARMS_REFRESH_MS, NULL);
}

/* A tap on the clock: this screen */
static void open_cb(lv_event_t *e)
{
    if (!s_screen) {
        screen_create();
    }
    fill();
    lv_scr_load(s_screen);
    lv_obj_scroll_to_y(s_table, 0, LV_ANIM_OFF);
}

void ui_alarms_init(void)
{
    // A clickable zone over the clock and the date of the sleep screen: it takes the tap instead of the sleep panel
    // (whose tap wakes the assistant up)
    lv_obj_t *zone = lv_obj_create(ui_PanelSleep);
    lv_obj_remove_style_all(zone);
    lv_obj_set_size(zone, CLOCK_ZONE_W, CLOCK_ZONE_H);
    lv_obj_align(zone, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(zone, open_cb, LV_EVENT_CLICKED, NULL);
}

void ui_alarms_close(void)
{
    if (s_screen && lv_scr_act() == s_screen) {
        lv_scr_load(ui_ScreenListen);
    }
}
