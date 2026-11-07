/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * The "Système" page of the Settings screen: see app_ui_system.h. Drawn by the LVGL task (no float in a printf: its
 * stack is small).
 */

#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "esp_spiffs.h"
#include "esp_littlefs.h"
#include "esp_timer.h"
#include "nvs.h"
#include "lvgl.h"
#include "file_lang.h"
#include "app_battery.h"
#include "app_ui_system.h"

LV_FONT_DECLARE(ui_font_Latin14);

#define SYSTEM_ROWS         14
#define SYSTEM_DIM          0x8A97BC

/* main.c: why the device started */
extern const char *assistant_reset_reason(void);

static void row(lv_obj_t *table, int *r, const char *name, const char *value)
{
    lv_table_set_cell_value(table, *r, 0, name);
    lv_table_set_cell_value(table, *r, 1, value);
    (*r)++;
}

void ui_system_fill(lv_obj_t *table)
{
    const app_lang_t *lang = app_lang_get();
    char text[96];
    int r = 0;

    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(text, sizeof(text), "%s (%s)", app->version, app->date);
    row(table, &r, lang->system_firmware, text);

    int mv = 0, percent = 0;
    app_battery_state_t state;
    row(table, &r, lang->system_board, app_battery_info(&mv, &percent, &state) ? "ESP32-S3-BOX-3 + BOX-3-SENSOR" :
        "ESP32-S3-BOX-3");

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    snprintf(text, sizeof(text), lang->system_chip_fmt, "ESP32-S3", (unsigned)(chip.revision / 100),
             (unsigned)(chip.revision % 100), (unsigned)chip.cores, (unsigned)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    row(table, &r, lang->system_chip, text);

    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    snprintf(text, sizeof(text), lang->system_mb, (unsigned)(flash >> 20));
    row(table, &r, lang->system_flash, text);

    snprintf(text, sizeof(text), lang->system_mb_free, (unsigned)(esp_psram_get_size() >> 20),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 20));
    row(table, &r, lang->system_psram, text);

    snprintf(text, sizeof(text), lang->system_kb_free, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) >> 10));
    row(table, &r, lang->system_ram, text);

    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        snprintf(text, sizeof(text), lang->system_kb_used, (unsigned)(used >> 10), (unsigned)(total >> 10));
        row(table, &r, lang->system_files, text);
    }
    if (esp_littlefs_info("state", &total, &used) == ESP_OK) {
        snprintf(text, sizeof(text), lang->system_kb_used, (unsigned)(used >> 10), (unsigned)(total >> 10));
        row(table, &r, lang->system_state, text);
    }
    nvs_stats_t nvs;
    if (nvs_get_stats("nvs", &nvs) == ESP_OK) {
        snprintf(text, sizeof(text), lang->system_entries, (unsigned)nvs.used_entries, (unsigned)nvs.total_entries);
        row(table, &r, lang->system_nvs, text);
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    snprintf(text, sizeof(text), "%s, %s", app->idf_ver, running ? running->label : "?");
    row(table, &r, lang->system_idf, text);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    row(table, &r, lang->system_mac, text);

    unsigned s = (unsigned)(esp_timer_get_time() / 1000000);
    snprintf(text, sizeof(text), lang->system_uptime_fmt, s / 86400, s / 3600 % 24, s / 60 % 60);
    row(table, &r, lang->system_uptime, text);

    row(table, &r, lang->system_reset, assistant_reset_reason());

    // The rows left over emptied first: LVGL 8 frees the cells of the rows it drops, and crashes on one never set
    for (uint16_t i = r; i < lv_table_get_row_cnt(table); i++) {
        lv_table_set_cell_value(table, i, 0, "");
        lv_table_set_cell_value(table, i, 1, "");
    }
    lv_table_set_row_cnt(table, r);
}

/* The names dim, the values white */
static void table_draw_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->part == LV_PART_ITEMS && dsc->label_dsc && dsc->id % 2 == 0) {
        dsc->label_dsc->color = lv_color_hex(SYSTEM_DIM);
    }
}

lv_obj_t *ui_system_create(lv_obj_t *parent)
{
    lv_obj_t *table = lv_table_create(parent);
    lv_table_set_col_cnt(table, 2);
    lv_table_set_row_cnt(table, SYSTEM_ROWS);
    for (int i = 0; i < SYSTEM_ROWS; i++) {
        lv_table_set_cell_value(table, i, 0, "");
        lv_table_set_cell_value(table, i, 1, "");
    }
    lv_table_set_col_width(table, 0, 104);
    lv_table_set_col_width(table, 1, 196);
    lv_obj_set_style_text_font(table, &ui_font_Latin14, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(table, lv_color_hex(0xFFFFFF), LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(table, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(table, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(table, 1, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(table, lv_color_hex(0x5A6A9A), LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(table, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(table, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(table, 3, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(table, 3, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(table, 4, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(table, 4, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_size(table, lv_pct(100), 124);
    lv_obj_set_scroll_dir(table, LV_DIR_VER);
    lv_obj_add_event_cb(table, table_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    return table;
}
