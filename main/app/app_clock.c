/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "app_clock.h"
#include "app_volume.h"
#include "app_wifi.h"
#include "app_battery.h"
#include "esp_wifi.h"

LV_FONT_DECLARE(ui_font_Latin16);

/* Seven-segment digit geometry (pixels) */
#define DIGIT_W         20
#define DIGIT_H         36
#define SEG_T           5       /* segment thickness */
#define SEG_GAP         1       /* gap between segments, gives the segmented look */
#define DIGIT_SPACING   6
#define COLON_W         14
#define DOT_SIZE        5
#define GLOW_WIDTH      10

/* Position: bottom of the digits and of the date, from the bottom of the screen */
#define CLOCK_BOTTOM_Y  (-26)
#define DATE_BOTTOM_Y   (-4)

/* Colours: pale blue-white segments with a blue glow, like the robot's eyes */
#define COLOR_ON        0xDDE9FF
#define COLOR_GLOW      0x4F86FF
#define COLOR_OFF       0x26305A
#define COLOR_DATE      0xB9CCF5

/*
 * Volume indicator, bottom left: gauge (outline of the full bar, filled from the bottom with the volume),
 * speaker icon, percentage
 */
#define VOLUME_CENTER_X     41      /* below the centre of the Settings icon (48 px wide, 17 px from the left) */
#define VOLUME_COLUMN_W     48
#define VOLUME_COLUMN_X     (VOLUME_CENTER_X - VOLUME_COLUMN_W / 2)     /* from the screen's left edge */
#define VOLUME_BAR_W        12
#define VOLUME_BAR_H        75
#define VOLUME_BAR_BORDER   2
#define VOLUME_BAR_GAP      1       /* between the outline and the filling */
#define VOLUME_BAR_BOTTOM_Y (-48)
#define VOLUME_ICON_BOTTOM_Y (-24)

/* Wi-Fi indicator, bottom right: dot and 3 arcs lit with the signal strength, crossed out when disconnected */
#define WIFI_W              56
#define WIFI_H              32
#define WIFI_RIGHT_X        (-6)
#define WIFI_BOTTOM_Y       (-28)   /* centred on the digits */
#define WIFI_ARC_WIDTH      4
#define WIFI_ARC_STEP       7       /* radius of the 1st arc, then 2nd, 3rd... */
#define WIFI_DOT_RADIUS     3
#define WIFI_GLOW_EXTRA     6       /* the glow of an arc is a wider, faint arc behind it */
/* RSSI (dBm) needed for 3, 2 and 1 lit arcs */
#define WIFI_RSSI_3         (-55)
#define WIFI_RSSI_2         (-67)
#define WIFI_RSSI_1         (-78)
#define WIFI_DISCONNECTED   (-1)

/*
 * Battery of the BOX-3-SENSOR dock, under the Wi-Fi: a frame, its tip and 5 sections, green when lit; only the last
 * one, red, when it is almost empty. Hidden without the dock or its cell.
 */
#define BATTERY_W           30
#define BATTERY_H           14
#define BATTERY_TIP_W       3
#define BATTERY_RIGHT_X     (WIFI_RIGHT_X - (WIFI_W - BATTERY_W - BATTERY_TIP_W) / 2 - BATTERY_TIP_W)
#define BATTERY_BOTTOM_Y    (-6)
#define BATTERY_GREEN       0x3DDC84
#define BATTERY_RED         0xFF4D4D
#define BATTERY_REFRESH_S   30

/* Valid once the year is past this one: the clock was synchronized */
#define SYNCED_MIN_YEAR 2024

static const char *TAG = "clock";

/* Segments a..g of each character, bit 0 = a ... bit 6 = g */
static const uint8_t s_digit_segments[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};
#define SEGMENTS_DASH   0x40

/* Segments (relative to the clock box) and what is lit; drawn by one object to spare internal RAM */
static lv_area_t s_segment_areas[4][7];
static lv_area_t s_dot_areas[2];
static uint8_t s_lit[4];
static lv_obj_t *s_box;
static lv_obj_t *s_date;
static const char *const *s_weekdays;
static int s_shown_minute = -1;
static lv_obj_t *s_volume_bar;
static lv_obj_t *s_volume_icon;
static lv_obj_t *s_volume_text;
static int s_shown_volume = -1;
static lv_obj_t *s_wifi;
static lv_obj_t *s_battery;
static lv_obj_t *s_battery_tip;
static lv_obj_t *s_battery_cells[APP_BATTERY_SECTIONS];
static int s_wifi_level = -2;   /* lit arcs 0..3, WIFI_DISCONNECTED, -2 not shown yet */

void app_clock_sync_start(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t ret = esp_netif_sntp_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init failed (0x%x)", ret);
    }
}

static void set_area(lv_area_t *a, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    a->x1 = x;
    a->y1 = y;
    a->x2 = x + w - 1;
    a->y2 = y + h - 1;
}

static void digit_layout(lv_coord_t x0, lv_area_t seg[7])
{
    const lv_coord_t half = DIGIT_H / 2;
    const lv_coord_t hor_w = DIGIT_W - 2 * (SEG_T / 2 + SEG_GAP);
    const lv_coord_t ver_h = half - SEG_T / 2 - 2 * SEG_GAP;
    const lv_coord_t hor_x = x0 + SEG_T / 2 + SEG_GAP;

    set_area(&seg[0], hor_x, 0, hor_w, SEG_T);                                   /* a */
    set_area(&seg[1], x0 + DIGIT_W - SEG_T, SEG_T / 2 + SEG_GAP, SEG_T, ver_h);   /* b */
    set_area(&seg[2], x0 + DIGIT_W - SEG_T, half + SEG_GAP, SEG_T, ver_h);        /* c */
    set_area(&seg[3], hor_x, DIGIT_H - SEG_T, hor_w, SEG_T);                      /* d */
    set_area(&seg[4], x0, half + SEG_GAP, SEG_T, ver_h);                          /* e */
    set_area(&seg[5], x0, SEG_T / 2 + SEG_GAP, SEG_T, ver_h);                     /* f */
    set_area(&seg[6], hor_x, half - SEG_T / 2, hor_w, SEG_T);                     /* g */
}

static void draw_segment(lv_draw_ctx_t *draw_ctx, const lv_area_t *origin, const lv_area_t *rel,
                         const lv_draw_rect_dsc_t *dsc)
{
    lv_area_t a = *rel;
    lv_area_move(&a, origin->x1, origin->y1);
    lv_draw_rect(draw_ctx, dsc, &a);
}

static void clock_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_REFR_EXT_DRAW_SIZE) {
        // Room for the glow around the segments
        lv_event_set_ext_draw_size(e, GLOW_WIDTH);
        return;
    }
    if (code != LV_EVENT_DRAW_MAIN) {
        return;
    }
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    lv_area_t origin;
    lv_obj_get_coords(s_box, &origin);

    lv_draw_rect_dsc_t on;
    lv_draw_rect_dsc_init(&on);
    on.radius = SEG_T / 2;
    on.bg_color = lv_color_hex(COLOR_ON);
    on.shadow_color = lv_color_hex(COLOR_GLOW);
    on.shadow_width = GLOW_WIDTH;
    on.shadow_opa = LV_OPA_70;

    lv_draw_rect_dsc_t off;
    lv_draw_rect_dsc_init(&off);
    off.radius = SEG_T / 2;
    off.bg_color = lv_color_hex(COLOR_OFF);

    for (int d = 0; d < 4; d++) {
        for (int i = 0; i < 7; i++) {
            draw_segment(draw_ctx, &origin, &s_segment_areas[d][i], (s_lit[d] & (1 << i)) ? &on : &off);
        }
    }
    on.radius = LV_RADIUS_CIRCLE;
    for (int i = 0; i < 2; i++) {
        draw_segment(draw_ctx, &origin, &s_dot_areas[i], &on);
    }
}

static void digit_show(int digit, uint8_t segments)
{
    s_lit[digit] = segments;
}

/* Volume gauge: outline of the full bar, filled from the bottom with the glow of the digits */
static void volume_bar_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_REFR_EXT_DRAW_SIZE) {
        lv_event_set_ext_draw_size(e, GLOW_WIDTH);
        return;
    }
    if (code != LV_EVENT_DRAW_MAIN) {
        return;
    }
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    lv_area_t area;
    lv_obj_get_coords(s_volume_bar, &area);

    lv_draw_rect_dsc_t outline;
    lv_draw_rect_dsc_init(&outline);
    outline.radius = VOLUME_BAR_W / 2;
    outline.bg_opa = LV_OPA_TRANSP;
    outline.border_color = lv_color_hex(COLOR_DATE);
    outline.border_opa = LV_OPA_70;
    outline.border_width = VOLUME_BAR_BORDER;
    lv_draw_rect(draw_ctx, &outline, &area);

    // Filling: inside the outline, its height in proportion to the volume
    const lv_coord_t inset = VOLUME_BAR_BORDER + VOLUME_BAR_GAP;
    area.x1 += inset;
    area.x2 -= inset;
    area.y1 += inset;
    area.y2 -= inset;
    int level = s_shown_volume < 0 ? 0 : s_shown_volume;
    lv_coord_t lit = (lv_coord_t)(lv_area_get_height(&area) * level / 100);
    if (lit > 0) {
        lv_draw_rect_dsc_t on;
        lv_draw_rect_dsc_init(&on);
        on.radius = lv_area_get_width(&area) / 2;
        on.bg_color = lv_color_hex(COLOR_ON);
        on.shadow_color = lv_color_hex(COLOR_GLOW);
        on.shadow_width = GLOW_WIDTH;
        on.shadow_opa = LV_OPA_70;
        area.y1 = area.y2 - lit + 1;
        lv_draw_rect(draw_ctx, &on, &area);
    }
}

static void volume_refresh(void)
{
    int level = app_volume_get();
    if (level == s_shown_volume) {
        return;
    }
    s_shown_volume = level;
    lv_label_set_text(s_volume_icon, level == 0 ? LV_SYMBOL_MUTE : (level < 50 ? LV_SYMBOL_VOLUME_MID :
                                                                    LV_SYMBOL_VOLUME_MAX));
    lv_label_set_text_fmt(s_volume_text, "%d %%", level);
    lv_obj_invalidate(s_volume_bar);
}

/* Wi-Fi symbol: a dot and 3 arcs opening upwards (LVGL angles: 0 on the right, clockwise, 270 at the top) */
static void wifi_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) {
        return;
    }
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    lv_area_t area;
    lv_obj_get_coords(s_wifi, &area);
    lv_point_t center = { area.x1 + WIFI_W / 2, area.y2 - WIFI_DOT_RADIUS - 1 };
    bool connected = (s_wifi_level != WIFI_DISCONNECTED);

    for (int i = 1; i <= 3; i++) {
        bool lit = connected && i <= s_wifi_level;
        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.rounded = 1;
        if (lit) {
            arc.color = lv_color_hex(COLOR_GLOW);
            arc.width = WIFI_ARC_WIDTH + WIFI_GLOW_EXTRA;
            arc.opa = LV_OPA_30;
            lv_draw_arc(draw_ctx, &arc, &center, i * WIFI_ARC_STEP + WIFI_GLOW_EXTRA / 2 + WIFI_ARC_WIDTH / 2,
                        225, 315);
        }
        arc.color = lv_color_hex(lit ? COLOR_ON : COLOR_OFF);
        arc.width = WIFI_ARC_WIDTH;
        arc.opa = LV_OPA_COVER;
        lv_draw_arc(draw_ctx, &arc, &center, i * WIFI_ARC_STEP + WIFI_ARC_WIDTH / 2, 225, 315);
    }

    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius = LV_RADIUS_CIRCLE;
    dot.bg_color = lv_color_hex(connected ? COLOR_ON : COLOR_OFF);
    if (connected) {
        dot.shadow_color = lv_color_hex(COLOR_GLOW);
        dot.shadow_width = GLOW_WIDTH / 2;
        dot.shadow_opa = LV_OPA_70;
    }
    lv_area_t dot_area = { center.x - WIFI_DOT_RADIUS, center.y - WIFI_DOT_RADIUS,
                           center.x + WIFI_DOT_RADIUS, center.y + WIFI_DOT_RADIUS };
    lv_draw_rect(draw_ctx, &dot, &dot_area);

    if (!connected) {
        // Crossed out, from the top left to the bottom right
        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.color = lv_color_hex(COLOR_DATE);
        line.width = 3;
        line.round_start = 1;
        line.round_end = 1;
        lv_point_t p1 = { area.x1 + 10, area.y1 + 2 };
        lv_point_t p2 = { area.x2 - 10, area.y2 - 1 };
        lv_draw_line(draw_ctx, &line, &p1, &p2);
    }
}

static void wifi_refresh(void)
{
    int level = WIFI_DISCONNECTED;
    wifi_ap_record_t ap;
    if (wifi_connected_already() == WIFI_STATUS_CONNECTED_OK && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        level = ap.rssi >= WIFI_RSSI_3 ? 3 : (ap.rssi >= WIFI_RSSI_2 ? 2 : (ap.rssi >= WIFI_RSSI_1 ? 1 : 0));
    }
    if (level != s_wifi_level) {
        s_wifi_level = level;
        lv_obj_invalidate(s_wifi);
    }
}

static void battery_refresh(void)
{
    static int countdown = 3;       // first read a few seconds after start-up (the dock is looked for meanwhile)
    if (--countdown > 0) {
        return;
    }
    countdown = BATTERY_REFRESH_S;
    bool critical = false;
    int lit = app_battery_sections(&critical);
    if (lit == 0) {
        lv_obj_add_flag(s_battery, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_battery_tip, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    // Sections from the left (the tip is on the right); almost empty: the first one only, in red
    for (int i = 0; i < APP_BATTERY_SECTIONS; i++) {
        bool on = critical ? (i == 0) : (i < lit);
        lv_obj_set_style_bg_color(s_battery_cells[i], lv_color_hex(on ? (critical ? BATTERY_RED : BATTERY_GREEN) :
                                  COLOR_OFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    lv_obj_clear_flag(s_battery, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_battery_tip, LV_OBJ_FLAG_HIDDEN);
}

static void battery_create(lv_obj_t *parent)
{
    s_battery = lv_obj_create(parent);
    lv_obj_remove_style_all(s_battery);
    lv_obj_set_size(s_battery, BATTERY_W, BATTERY_H);
    lv_obj_set_style_border_width(s_battery, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_battery, lv_color_hex(COLOR_DATE), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_battery, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_battery, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(s_battery, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(s_battery, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(s_battery, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_battery, LV_ALIGN_BOTTOM_RIGHT, BATTERY_RIGHT_X, BATTERY_BOTTOM_Y);
    for (int i = 0; i < APP_BATTERY_SECTIONS; i++) {
        s_battery_cells[i] = lv_obj_create(s_battery);
        lv_obj_remove_style_all(s_battery_cells[i]);
        lv_obj_set_flex_grow(s_battery_cells[i], 1);
        lv_obj_set_height(s_battery_cells[i], lv_pct(100));
        lv_obj_set_style_bg_opa(s_battery_cells[i], LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(s_battery_cells[i], lv_color_hex(COLOR_OFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(s_battery_cells[i], 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    s_battery_tip = lv_obj_create(parent);
    lv_obj_remove_style_all(s_battery_tip);
    lv_obj_set_size(s_battery_tip, BATTERY_TIP_W, BATTERY_H / 2);
    lv_obj_set_style_bg_opa(s_battery_tip, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_battery_tip, lv_color_hex(COLOR_DATE), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_battery_tip, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align_to(s_battery_tip, s_battery, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
    lv_obj_add_flag(s_battery, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_battery_tip, LV_OBJ_FLAG_HIDDEN);
}

static void clock_refresh(lv_timer_t *timer)
{
    volume_refresh();
    wifi_refresh();
    battery_refresh();

    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    bool synced = t.tm_year + 1900 >= SYNCED_MIN_YEAR;
    int minute = synced ? t.tm_hour * 60 + t.tm_min : -2;
    if (minute == s_shown_minute) {
        return;
    }
    s_shown_minute = minute;

    if (!synced) {
        // Not synchronized yet: "--:--" and no date
        for (int d = 0; d < 4; d++) {
            digit_show(d, SEGMENTS_DASH);
        }
        lv_label_set_text(s_date, "");
        lv_obj_invalidate(s_box);
        return;
    }
    digit_show(0, s_digit_segments[t.tm_hour / 10]);
    digit_show(1, s_digit_segments[t.tm_hour % 10]);
    digit_show(2, s_digit_segments[t.tm_min / 10]);
    digit_show(3, s_digit_segments[t.tm_min % 10]);
    lv_obj_invalidate(s_box);
    lv_label_set_text_fmt(s_date, "%s %02d/%02d/%02d", s_weekdays[t.tm_wday], t.tm_mday, t.tm_mon + 1,
                          t.tm_year % 100);
}

void app_clock_show(bool shown)
{
    if (!s_box) {
        return;
    }
    if (shown) {
        lv_obj_clear_flag(s_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_date, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_date, LV_OBJ_FLAG_HIDDEN);
    }
}

void app_clock_set_weekdays(const char *const weekdays[7])
{
    s_weekdays = weekdays;
    s_shown_minute = -1;    // the date is written again at the next refresh
    clock_refresh(NULL);
}

void app_clock_create(lv_obj_t *parent, const char *const weekdays[7])
{
    s_weekdays = weekdays;

    const lv_coord_t width = 4 * DIGIT_W + 2 * DIGIT_SPACING + COLON_W;
    s_box = lv_obj_create(parent);
    lv_obj_remove_style_all(s_box);
    lv_obj_clear_flag(s_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_box, width, DIGIT_H);
    lv_obj_align(s_box, LV_ALIGN_BOTTOM_MID, 0, CLOCK_BOTTOM_Y);
    lv_obj_add_event_cb(s_box, clock_event_cb, LV_EVENT_ALL, NULL);

    lv_coord_t x = 0;
    for (int d = 0; d < 4; d++) {
        digit_layout(x, s_segment_areas[d]);
        x += DIGIT_W + (d == 1 ? COLON_W : DIGIT_SPACING);
    }
    // Colon between the hours and the minutes
    lv_coord_t colon_x = 2 * DIGIT_W + DIGIT_SPACING + (COLON_W - DOT_SIZE) / 2;
    set_area(&s_dot_areas[0], colon_x, DIGIT_H / 3 - DOT_SIZE / 2, DOT_SIZE, DOT_SIZE);
    set_area(&s_dot_areas[1], colon_x, 2 * DIGIT_H / 3 - DOT_SIZE / 2, DOT_SIZE, DOT_SIZE);
    lv_obj_refresh_ext_draw_size(s_box);

    s_date = lv_label_create(parent);
    lv_obj_set_style_text_font(s_date, &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_date, lv_color_hex(COLOR_DATE), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(s_date, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_date, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_date, LV_ALIGN_BOTTOM_MID, 0, DATE_BOTTOM_Y);
    lv_label_set_text(s_date, "");

    // Volume: bar, speaker icon (symbol font) and percentage, centred in a column at the bottom left
    s_volume_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(s_volume_bar);
    lv_obj_clear_flag(s_volume_bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_volume_bar, VOLUME_BAR_W, VOLUME_BAR_H);
    lv_obj_align(s_volume_bar, LV_ALIGN_BOTTOM_LEFT, VOLUME_CENTER_X - VOLUME_BAR_W / 2, VOLUME_BAR_BOTTOM_Y);
    lv_obj_add_event_cb(s_volume_bar, volume_bar_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_refresh_ext_draw_size(s_volume_bar);

    s_volume_icon = lv_label_create(parent);
    lv_obj_set_width(s_volume_icon, VOLUME_COLUMN_W);
    lv_obj_set_style_text_font(s_volume_icon, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_volume_icon, lv_color_hex(COLOR_DATE), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_volume_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_volume_icon, LV_ALIGN_BOTTOM_LEFT, VOLUME_COLUMN_X, VOLUME_ICON_BOTTOM_Y);

    s_volume_text = lv_label_create(parent);
    lv_obj_set_width(s_volume_text, VOLUME_COLUMN_W);
    lv_obj_set_style_text_font(s_volume_text, &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_volume_text, lv_color_hex(COLOR_DATE), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_volume_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_volume_text, LV_ALIGN_BOTTOM_LEFT, VOLUME_COLUMN_X, DATE_BOTTOM_Y);

    // Wi-Fi signal at the bottom right
    s_wifi = lv_obj_create(parent);
    lv_obj_remove_style_all(s_wifi);
    lv_obj_clear_flag(s_wifi, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_wifi, WIFI_W, WIFI_H);
    lv_obj_align(s_wifi, LV_ALIGN_BOTTOM_RIGHT, WIFI_RIGHT_X, WIFI_BOTTOM_Y);
    lv_obj_add_event_cb(s_wifi, wifi_event_cb, LV_EVENT_DRAW_MAIN, NULL);

    // The battery of the BOX-3-SENSOR dock, under the Wi-Fi
    battery_create(parent);

    clock_refresh(NULL);
    lv_timer_create(clock_refresh, 1000, NULL);
}
