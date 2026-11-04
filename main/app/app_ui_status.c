/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Screen of the Wi-Fi and the battery: see app_ui_status.h. Drawn by the LVGL task (no float in a printf: its
 * stack is small).
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lvgl.h"
#include "ui.h"
#include "file_lang.h"
#include "app_wifi.h"
#include "app_battery.h"
#include "app_ui_status.h"

LV_FONT_DECLARE(ui_font_Latin14);
LV_FONT_DECLARE(ui_font_Latin20);

#define STATUS_REFRESH_MS   2000
#define STATUS_ROWS         14
#define STATUS_DIM          0x8A97BC

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_table = NULL;
static lv_timer_t *s_timer = NULL;

static const char *auth_name(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:            return "-";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2 Enterprise";
    default:                        return "?";
    }
}

/* A row: its name on the left (dim), its value on the right; a section's title alone, in white */
static void row(int *r, const char *name, const char *value)
{
    lv_table_set_cell_value(s_table, *r, 0, name);
    lv_table_set_cell_value(s_table, *r, 1, value ? value : "");
    (*r)++;
}

static void fill(void)
{
    const app_lang_t *lang = app_lang_get();
    char text[64];
    int r = 0;
    lv_label_set_text(s_title, lang->status_title);

    // The network
    row(&r, lang->status_wifi, "");
    wifi_ap_record_t ap;
    if (wifi_connected_already() == WIFI_STATUS_CONNECTED_OK && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        row(&r, lang->status_ssid, (const char *)ap.ssid);
        // dBm, and the quality as % (-100 dBm: 0, -50 dBm and more: 100)
        int quality = 2 * (ap.rssi + 100);
        quality = quality < 0 ? 0 : (quality > 100 ? 100 : quality);
        snprintf(text, sizeof(text), "%d dBm (%d %%)", ap.rssi, quality);
        row(&r, lang->status_signal, text);
        snprintf(text, sizeof(text), "%d (2.4 GHz%s%s)", ap.primary, ap.phy_11n ? ", 802.11n" : "",
                 ap.second == WIFI_SECOND_CHAN_NONE ? "" : ", 40 MHz");
        row(&r, lang->status_channel, text);
        snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", ap.bssid[0], ap.bssid[1], ap.bssid[2],
                 ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        row(&r, lang->status_ap, text);
        row(&r, lang->status_security, auth_name(ap.authmode));
        esp_netif_ip_info_t ip;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
            snprintf(text, sizeof(text), IPSTR, IP2STR(&ip.ip));
            row(&r, lang->status_ip, text);
            snprintf(text, sizeof(text), IPSTR, IP2STR(&ip.gw));
            row(&r, lang->status_gateway, text);
        }
    } else {
        row(&r, lang->status_ssid, lang->status_not_connected);
    }
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        row(&r, lang->status_mac, text);
    }

    // The battery
    row(&r, lang->status_battery, "");
    int mv = 0, percent = 0;
    app_battery_state_t state;
    if (app_battery_info(&mv, &percent, &state)) {
        snprintf(text, sizeof(text), "%d %%", percent);
        row(&r, lang->status_level, text);
        snprintf(text, sizeof(text), "%d,%02d V", mv / 1000, (mv % 1000) / 10);
        row(&r, lang->status_voltage, text);
        const char *states[] = {
            [APP_BATTERY_UNKNOWN] = lang->status_unknown, [APP_BATTERY_CHARGING] = lang->status_charging,
            [APP_BATTERY_DISCHARGING] = lang->status_discharging, [APP_BATTERY_FULL] = lang->status_full,
            [APP_BATTERY_STEADY] = lang->status_steady,
        };
        row(&r, lang->status_state, states[state]);
    } else {
        row(&r, lang->status_no_battery, "");
    }
    lv_table_set_row_cnt(s_table, r);
}

/* The rows naming a section (Wi-Fi, battery) in white, on the whole width; the others' names dim */
static void table_draw_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->part != LV_PART_ITEMS || !dsc->label_dsc) {
        return;
    }
    uint32_t r = dsc->id / 2, c = dsc->id % 2;
    const char *value = lv_table_get_cell_value(s_table, r, 1);
    bool section = (value && !value[0]);
    if (c == 0) {
        dsc->label_dsc->color = lv_color_hex(section ? 0xFFFFFF : STATUS_DIM);
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
    lv_table_set_row_cnt(s_table, STATUS_ROWS);
    lv_table_set_col_width(s_table, 0, 116);
    lv_table_set_col_width(s_table, 1, 188);
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

    s_timer = lv_timer_create(refresh_cb, STATUS_REFRESH_MS, NULL);
}

/* A tap on the Wi-Fi or the battery: this screen, scrolled to what was tapped */
static void open_cb(lv_event_t *e)
{
    bool battery = (bool)(intptr_t)lv_event_get_user_data(e);
    if (!s_screen) {
        screen_create();
    }
    fill();
    lv_scr_load(s_screen);
    // The battery is at the end
    lv_obj_update_layout(s_table);
    lv_obj_scroll_to_y(s_table, battery ? LV_COORD_MAX : 0, LV_ANIM_OFF);
}

void ui_status_init(void)
{
    // Clickable zones over the Wi-Fi (above) and the battery (below) of the sleep screen: they take the tap instead of
    // the sleep panel (whose tap wakes the assistant up)
    for (int i = 0; i < 2; i++) {
        lv_obj_t *zone = lv_obj_create(ui_PanelSleep);
        lv_obj_remove_style_all(zone);
        lv_obj_set_size(zone, 70, i ? 30 : 46);
        lv_obj_align(zone, LV_ALIGN_BOTTOM_RIGHT, -4, i ? 0 : -30);
        lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(zone, open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

void ui_status_close(void)
{
    if (s_screen && lv_scr_act() == s_screen) {
        lv_scr_load(ui_ScreenListen);
    }
}
