/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_system.h"

#include "app_ui_ctrl.h"
#include "app_wifi.h"
#include "app_lang.h"
#include "llm.h"
#include "app_clock.h"
#include "settings.h"
#include "bsp/esp-bsp.h"

#include "ui_helpers.h"
#include "ui.h"

LV_FONT_DECLARE(ui_font_Latin14);
LV_FONT_DECLARE(ui_font_Latin16);
LV_FONT_DECLARE(ui_font_Latin20);

#define LABEL_WIFI_DOT_COUNT_MAX        (10)
#define WIFI_CHECK_TIMER_INTERVAL_S     (1)
#define REPLY_SCROLL_TIMER_INTERVAL_MS  (1000)
#define REPLY_SCROLL_SPEED              (1)
/* Text under the robot (listening / thinking): lines shown, bottom position (from the screen bottom) */
#define LISTEN_TEXT_LINES               (2)
#define LISTEN_TEXT_BOTTOM_Y            (-8)
/* The robot is raised to leave room for these lines */
#define ROBOT_RAISE_PX                  (20)

static char *TAG = "ui_ctrl";

static ui_ctrl_panel_t current_panel = UI_CTRL_PANEL_SLEEP;
static lv_timer_t *scroll_timer_handle = NULL;
static bool reply_audio_start = false;
static bool reply_audio_end = false;
static bool reply_content_get = false;
static uint16_t content_height = 0;

static void reply_content_scroll_timer_handler();
static void wifi_check_timer_handler(lv_timer_t *timer);

static void label_localize(lv_obj_t *label, const char *text, const lv_font_t *font)
{
    if (text == NULL) {
        return;
    }
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(label, text);
}

/*
 * Move a robot part up. The y set by the generated code is read from the style: lv_obj_get_y() gives the
 * position on screen, which is not computed yet when the screens have just been created.
 */
static void robot_raise(lv_obj_t *obj)
{
    lv_obj_set_y(obj, lv_obj_get_style_y(obj, LV_PART_MAIN) - ROBOT_RAISE_PX);
}

/* Text under "Listening": show its last line, scrolling up smoothly when the text grows */
static void listen_label_set_text(const char *text)
{
    lv_label_set_text(ui_LabelListenSpeak, text);
    lv_obj_update_layout(ui_LabelListenSpeak);
    lv_coord_t bottom = lv_obj_get_scroll_bottom(ui_LabelListenSpeak);
    if (bottom > 0) {
        lv_obj_scroll_by(ui_LabelListenSpeak, 0, -bottom, LV_ANIM_ON);
    } else if (bottom < 0) {
        // Shorter text (new session): back to the top at once
        lv_obj_scroll_to_y(ui_LabelListenSpeak, 0, LV_ANIM_OFF);
    }
}

/* ---------- Settings screen: language and model, saved to NVS with the Save button, then restart ---------- */

#define SETTINGS_RESTART_DELAY_MS       1000
#define SETTINGS_OPTIONS_SIZE           256

/* Languages offered, in the order of the dropdown */
static const char *const s_settings_languages[] = { "fr", "en" };

static lv_obj_t *s_model_dropdown = NULL;
static lv_obj_t *s_save_btn = NULL;
/* Selections matching the stored settings */
static uint16_t s_lang_stored = 0;
static uint16_t s_model_stored = 0;

static void settings_restart_timer_cb(lv_timer_t *timer)
{
    esp_restart();
}

/* Show the confirmation over the screen, then restart so that the new settings are used everywhere */
static void settings_saved(void)
{
    lv_obj_t *overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
    lv_obj_center(overlay);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(overlay, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_80, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *label = lv_label_create(overlay);
    lv_label_set_text(label, app_lang_get()->settings_saved);
    lv_obj_set_style_text_font(label, &ui_font_Latin20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(label);

    lv_timer_create(settings_restart_timer_cb, SETTINGS_RESTART_DELAY_MS, NULL);
}

/* Enable the Save button when the selection differs from the stored settings */
static void settings_changed_cb(lv_event_t *e)
{
    bool changed = lv_dropdown_get_selected(ui_DropdownSettingsRegion) != s_lang_stored ||
                   lv_dropdown_get_selected(s_model_dropdown) != s_model_stored;
    if (changed) {
        lv_obj_clear_state(s_save_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_save_btn, LV_STATE_DISABLED);
    }
}

static void settings_save_clicked_cb(lv_event_t *e)
{
    esp_err_t ret = ESP_OK;
    uint16_t lang = lv_dropdown_get_selected(ui_DropdownSettingsRegion);
    if (lang != s_lang_stored && lang < sizeof(s_settings_languages) / sizeof(s_settings_languages[0])) {
        ret = settings_save_str("Language", s_settings_languages[lang]);
    }
    // The entry after the listed models is a model set in CONFIG.INI: already the stored one
    uint16_t model = lv_dropdown_get_selected(s_model_dropdown);
    if (ret == ESP_OK && model != s_model_stored && model < llm_model_count) {
        ret = settings_save_str("Chat_model", llm_models[model].id);
    }
    if (ret == ESP_OK) {
        settings_saved();
    }
}

static void settings_dropdown_setup(lv_obj_t *dropdown, const char *options, uint16_t selected)
{
    lv_dropdown_set_options(dropdown, options);
    lv_dropdown_set_selected(dropdown, selected);
    // Accented names: Latin font for the field and its list (the arrow keeps the default font and its symbol)
    lv_obj_set_style_text_font(dropdown, &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dropdown), &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(dropdown, settings_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* A row of the Settings panel: label on the left, dropdown on the right (same look as the generated row) */
static lv_obj_t *settings_row_create(const char *text, lv_coord_t dropdown_width)
{
    lv_obj_t *row = lv_obj_create(ui_PanelSettings);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, lv_pct(19));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(row, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(row, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(label, &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *dropdown = lv_dropdown_create(row);
    lv_obj_set_width(dropdown, dropdown_width);
    lv_obj_set_height(dropdown, lv_pct(100));
    lv_obj_set_style_text_align(dropdown, LV_TEXT_ALIGN_CENTER, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    return dropdown;
}

static void settings_screen_setup(void)
{
    const app_lang_t *lang = app_lang_get();
    const sys_param_t *param = settings_get_parameter();
    char options[SETTINGS_OPTIONS_SIZE];

    lv_obj_set_style_pad_row(ui_PanelSettings, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    // The generated "Region" row (unused) becomes the language row
    label_localize(ui_LabelSettingsRegion, lang->settings_language, &ui_font_Latin16);
    lv_obj_set_flex_align(ui_PanelSettingsRegion, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ui_PanelSettingsRegion, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_PanelSettingsRegion, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_width(ui_DropdownSettingsRegion, lv_pct(40));
    uint16_t lang_selected = 1;   // English UI for other languages
    for (uint16_t i = 0; i < sizeof(s_settings_languages) / sizeof(s_settings_languages[0]); i++) {
        if (strcasecmp(param->lang, s_settings_languages[i]) == 0) {
            lang_selected = i;
        }
    }
    s_lang_stored = lang_selected;
    settings_dropdown_setup(ui_DropdownSettingsRegion, "Français\nEnglish", lang_selected);

    // Model row: the listed models, plus the current one if it was set in CONFIG.INI and is not listed
    s_model_dropdown = settings_row_create(lang->settings_model, lv_pct(62));
    options[0] = '\0';
    uint16_t model_selected = llm_model_count;
    for (size_t i = 0; i < llm_model_count; i++) {
        strlcat(options, i ? "\n" : "", sizeof(options));
        strlcat(options, llm_models[i].label, sizeof(options));
        if (strcmp(param->chat_model, llm_models[i].id) == 0) {
            model_selected = i;
        }
    }
    if (model_selected == llm_model_count) {
        strlcat(options, "\n", sizeof(options));
        strlcat(options, param->chat_model, sizeof(options));
    }
    s_model_stored = model_selected;
    settings_dropdown_setup(s_model_dropdown, options, model_selected);

    // Save button (bottom right), enabled once a selection differs from the stored settings
    s_save_btn = lv_btn_create(ui_ScreenSettings);
    lv_obj_align(s_save_btn, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_set_style_bg_color(s_save_btn, lv_color_hex(0x6E47BD), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_state(s_save_btn, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_save_btn, settings_save_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_label = lv_label_create(s_save_btn);
    lv_label_set_text(save_label, lang->settings_save);
    lv_obj_set_style_text_font(save_label, &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(save_label);
}

/* Override the texts of the SquareLine-generated screens without editing the generated files */
static void ui_ctrl_localize(void)
{
    const app_lang_t *lang = app_lang_get();

    // Generated UI fonts lack accented glyphs: use the Latin-1 fonts (CJK falls back to KaiTi)
    lv_obj_set_style_text_font(ui_LabelListenSpeak, &ui_font_Latin20, LV_PART_MAIN | LV_STATE_DEFAULT);
    /*
     * Text under "Listening": wrapped on several lines, one line shown, scrolled up to the last line as the
     * transcript grows (the generated label scrolls horizontally on a single line)
     */
    lv_label_set_long_mode(ui_LabelListenSpeak, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(ui_LabelListenSpeak, LISTEN_TEXT_LINES * lv_font_get_line_height(&ui_font_Latin20));
    lv_obj_set_y(ui_LabelListenSpeak, LISTEN_TEXT_BOTTOM_Y);
    // The robot (asleep, listening, thinking) and its shadow move up to make room for the text lines
    robot_raise(ui_ImageSleepBody);
    robot_raise(ui_ContainerBigZ);
    robot_raise(ui_ContainerSmallZ);
    robot_raise(ui_ImageListenBackGlow);
    robot_raise(ui_ImageGetBackGlow);
    robot_raise(ui_ImageBodyShadow);
    lv_obj_set_scroll_dir(ui_LabelListenSpeak, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui_LabelListenSpeak, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(ui_LabelListenSpeak, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(ui_LabelReplyQuestion, &ui_font_Latin20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_LabelReplyContent, &ui_font_Latin20, LV_PART_MAIN | LV_STATE_DEFAULT);
    label_localize(ui_LabelSetupWifi, lang->wifi_connecting, &ui_font_Latin20);

    label_localize(ui_LabelSetupStepTile, lang->steps_title, &ui_font_Latin20);
    label_localize(ui_LabelSetupStepContent, lang->steps_content, &ui_font_Latin14);
    label_localize(ui_LabelSettingsTile, lang->settings_title, &ui_font_Latin20);
    settings_screen_setup();
    label_localize(ui_LabelResetTitle, lang->reset_title, &ui_font_Latin20);
    label_localize(ui_LabelResetContent, lang->reset_content, &ui_font_Latin14);
    label_localize(ui_LabelWifiResetTitle, lang->reset_title, &ui_font_Latin20);
    label_localize(ui_LabelWifiResetContent, lang->reset_content, &ui_font_Latin14);
}

void ui_ctrl_init(void)
{
    bsp_display_lock(0);

    ui_init();
    ui_ctrl_localize();
    app_clock_create(ui_PanelSleep, app_lang_get()->weekdays);

    scroll_timer_handle = lv_timer_create(reply_content_scroll_timer_handler, REPLY_SCROLL_TIMER_INTERVAL_MS / REPLY_SCROLL_SPEED, NULL);
    lv_timer_pause(scroll_timer_handle);

    lv_timer_create(wifi_check_timer_handler, WIFI_CHECK_TIMER_INTERVAL_S * 1000, NULL);

    bsp_display_unlock();
}

static void wifi_check_timer_handler(lv_timer_t *timer)
{
    if (WIFI_STATUS_CONNECTED_OK == wifi_connected_already()) {
        lv_obj_clear_flag(ui_PanelSetupSteps, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_PanelSetupWifi, LV_OBJ_FLAG_HIDDEN);
        lv_timer_del(timer);
        if (ui_get_btn_op_group()) {
            lv_group_remove_all_objs(ui_get_btn_op_group());
            lv_group_add_obj(ui_get_btn_op_group(), ui_ButtonSetup);
        }
    } else if (WIFI_STATUS_CONNECTED_FAILED == wifi_connected_already()) {
        lv_label_set_text(ui_LabelSetupWifi, app_lang_get()->wifi_failed);
    } else {
        const char *connecting = app_lang_get()->wifi_connecting;
        if (strlen(lv_label_get_text(ui_LabelSetupWifi)) >= strlen(connecting) + LABEL_WIFI_DOT_COUNT_MAX) {
            lv_label_set_text(ui_LabelSetupWifi, connecting);
        } else {
            lv_label_ins_text(ui_LabelSetupWifi, LV_LABEL_POS_LAST, ".");
        }
    }
}

static void show_panel_timer_handler(struct _lv_timer_t *t)
{
    ui_ctrl_panel_t panel = (ui_ctrl_panel_t)t->user_data;
    lv_obj_t *show_panel = NULL;
    lv_obj_t *hide_panel[3] = { NULL };

    switch (panel) {
    case UI_CTRL_PANEL_SLEEP:
        show_panel = ui_PanelSleep;
        hide_panel[0] = ui_PanelListen;
        hide_panel[1] = ui_PanelGet;
        hide_panel[2] = ui_PanelReply;
        lv_obj_clear_flag(ui_ImageListenSettings, LV_OBJ_FLAG_HIDDEN);
        listen_label_set_text(" ");
        break;
    case UI_CTRL_PANEL_LISTEN:
        show_panel = ui_PanelListen;
        hide_panel[0] = ui_PanelSleep;
        hide_panel[1] = ui_PanelGet;
        hide_panel[2] = ui_PanelReply;
        lv_obj_clear_flag(ui_LabelListenSpeak, LV_OBJ_FLAG_HIDDEN);
        listen_label_set_text(app_lang_get()->listening);
        // Reset flags and timer of reply
        reply_content_get = false;
        reply_audio_start = false;
        reply_audio_end = false;
        lv_timer_pause(scroll_timer_handle);
        break;
    case UI_CTRL_PANEL_GET:
        show_panel = ui_PanelGet;
        hide_panel[0] = ui_PanelSleep;
        hide_panel[1] = ui_PanelListen;
        hide_panel[2] = ui_PanelReply;
        lv_obj_clear_flag(ui_LabelListenSpeak, LV_OBJ_FLAG_HIDDEN);
        listen_label_set_text(app_lang_get()->thinking);
        break;
    case UI_CTRL_PANEL_REPLY:
        show_panel = ui_PanelReply;
        hide_panel[0] = ui_PanelSleep;
        hide_panel[1] = ui_PanelListen;
        hide_panel[2] = ui_PanelGet;
        lv_obj_add_flag(ui_ImageListenSettings, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_ImageListenSettings, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_LabelListenSpeak, LV_OBJ_FLAG_HIDDEN);
        break;
    default:
        break;
    }

    if (panel != UI_CTRL_PANEL_REPLY) {
        lv_obj_clear_flag(ui_ImageListenSettings, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_clear_flag(show_panel, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < sizeof(hide_panel) / sizeof(lv_obj_t *); i++) {
        lv_obj_add_flag(hide_panel[i], LV_OBJ_FLAG_HIDDEN);
    }

    current_panel = panel;

    ESP_LOGI(TAG, "Swich to panel[%d]", panel);
}

void ui_ctrl_show_panel(ui_ctrl_panel_t panel, uint16_t timeout)
{
    bsp_display_lock(0);

    if (timeout) {
        lv_timer_t *timer = lv_timer_create(show_panel_timer_handler, timeout, NULL);
        timer->user_data = (void *)panel;
        lv_timer_set_repeat_count(timer, 1);
        ESP_LOGW(TAG, "Switch panel to [%d] in %dms", panel, timeout);
    } else {
        lv_timer_t timer;
        timer.user_data = (void *)panel;
        show_panel_timer_handler(&timer);
    }

    bsp_display_unlock();
}

static void reply_content_show_text(const char *text)
{
    if (NULL == text) {
        return;
    }

    char *decode = heap_caps_malloc((strlen(text) + 1), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(decode);

    int j = 0;
    for (int i = 0; i < strlen(text);) {
        if ((*(text + i) == '\\') && ((i + 1) < strlen(text)) && (*(text + i + 1) == 'n')) {
            *(decode + j++) = '\n';
            i += 2;
        } else {
            *(decode + j++) = *(text + i);
            i += 1;
        }
    }
    *(decode + j) = '\0';

    ESP_LOGI(TAG, "decode:[%d, %d] %s\r\n", j, strlen(decode), decode);

    lv_label_set_text(ui_LabelReplyContent, decode);
    content_height = lv_obj_get_self_height(ui_LabelReplyContent);
    lv_obj_scroll_to_y(ui_ContainerReplyContent, 0, LV_ANIM_OFF);
    reply_content_get = true;
    lv_timer_resume(scroll_timer_handle);
    ESP_LOGI(TAG, "reply scroll timer start");

    if (decode) {
        free(decode);
    }
}

void ui_ctrl_label_show_text(ui_ctrl_label_t label, const char *text)
{
    bsp_display_lock(0);

    if (text != NULL) {
        switch (label) {
        case UI_CTRL_LABEL_LISTEN_SPEAK:
            ESP_LOGI(TAG, "update listen speak");
            listen_label_set_text(text);
            break;
        case UI_CTRL_LABEL_REPLY_QUESTION:
            ESP_LOGI(TAG, "update reply question");
            lv_label_set_text(ui_LabelReplyQuestion, text);
            break;
        case UI_CTRL_LABEL_REPLY_CONTENT:
            ESP_LOGI(TAG, "update reply content");
            reply_content_show_text(text);
            break;
        default:
            break;
        }
    }

    bsp_display_unlock();
}

static void anim_callback_set_bg_img_opacity(lv_anim_t *a, int32_t v)
{
    ui_anim_user_data_t *usr = (ui_anim_user_data_t *)a->user_data;
    lv_obj_set_style_bg_img_opa(usr->target, v, 0);
}

static int32_t anim_callback_get_opacity(lv_anim_t *a)
{
    ui_anim_user_data_t *usr = (ui_anim_user_data_t *)a->user_data;
    return lv_obj_get_style_bg_img_opa(usr->target, 0);
}

void ui_sleep_show_animation(void)
{
    bsp_display_lock(0);

    ui_anim_user_data_t *PropertyAnimation_0_user_data = lv_mem_alloc(sizeof(ui_anim_user_data_t));
    PropertyAnimation_0_user_data->target = ui_ContainerBigZ;
    PropertyAnimation_0_user_data->val = -1;
    lv_anim_t PropertyAnimation_0;
    lv_anim_init(&PropertyAnimation_0);
    lv_anim_set_time(&PropertyAnimation_0, 1000);
    lv_anim_set_user_data(&PropertyAnimation_0, PropertyAnimation_0_user_data);
    lv_anim_set_custom_exec_cb(&PropertyAnimation_0, anim_callback_set_bg_img_opacity );
    lv_anim_set_values(&PropertyAnimation_0, 0, 255 );
    lv_anim_set_path_cb( &PropertyAnimation_0, lv_anim_path_linear);
    lv_anim_set_delay( &PropertyAnimation_0, 0 );
    // lv_anim_set_deleted_cb( &PropertyAnimation_0, _ui_anim_callback_free_user_data );
    lv_anim_set_playback_time(&PropertyAnimation_0, 1000);
    lv_anim_set_playback_delay(&PropertyAnimation_0, 0);
    lv_anim_set_repeat_count(&PropertyAnimation_0, LV_ANIM_REPEAT_INFINITE );
    lv_anim_set_repeat_delay(&PropertyAnimation_0, 1000);
    lv_anim_set_early_apply( &PropertyAnimation_0, false );
    lv_anim_set_get_value_cb(&PropertyAnimation_0, &anim_callback_get_opacity );
    lv_anim_start(&PropertyAnimation_0);

    ui_anim_user_data_t *PropertyAnimation_1_user_data = lv_mem_alloc(sizeof(ui_anim_user_data_t));
    PropertyAnimation_1_user_data->target = ui_ContainerSmallZ;
    PropertyAnimation_1_user_data->val = -1;
    lv_anim_t PropertyAnimation_1;
    lv_anim_init(&PropertyAnimation_1);
    lv_anim_set_time(&PropertyAnimation_1, 1000);
    lv_anim_set_user_data(&PropertyAnimation_1, PropertyAnimation_1_user_data);
    lv_anim_set_custom_exec_cb(&PropertyAnimation_1, anim_callback_set_bg_img_opacity );
    lv_anim_set_values(&PropertyAnimation_1, 0, 255 );
    lv_anim_set_path_cb( &PropertyAnimation_1, lv_anim_path_linear);
    lv_anim_set_delay( &PropertyAnimation_1, 1000 );
    // lv_anim_set_deleted_cb( &PropertyAnimation_1, _ui_anim_callback_free_user_data );
    lv_anim_set_playback_time(&PropertyAnimation_1, 1000);
    lv_anim_set_playback_delay(&PropertyAnimation_1, 0);
    lv_anim_set_repeat_count(&PropertyAnimation_1, LV_ANIM_REPEAT_INFINITE );
    lv_anim_set_repeat_delay(&PropertyAnimation_1, 1000);
    lv_anim_set_early_apply( &PropertyAnimation_1, false );
    lv_anim_set_get_value_cb(&PropertyAnimation_1, &anim_callback_get_opacity );
    lv_anim_start(&PropertyAnimation_1);

    bsp_display_unlock();
}

void ui_ctrl_reply_set_audio_start_flag(bool result)
{
    reply_audio_start = result;
}

bool ui_ctrl_reply_get_audio_start_flag(void)
{
    return reply_audio_start;
}

void ui_ctrl_reply_set_audio_end_flag(bool result)
{
    reply_audio_end = result;
}

static void reply_content_scroll_timer_handler()
{
    lv_coord_t offset = 0;
    const lv_font_t *font = NULL;

    if (reply_content_get && reply_audio_start) {
        font = lv_obj_get_style_text_font(ui_LabelReplyContent, 0);
        offset = lv_obj_get_scroll_y(ui_ContainerReplyContent);
        // ESP_LOGI(TAG, "offset: %d, content_height: %d, font_height: %d", offset, content_height, font->line_height);
        if ((content_height > lv_obj_get_height(ui_ContainerReplyContent)) &&
                (offset < (content_height - lv_obj_get_height(ui_ContainerReplyContent)))) {
            offset += font->line_height / 2;
            lv_obj_scroll_to_y(ui_ContainerReplyContent, offset, LV_ANIM_OFF);
        } else if (reply_audio_end) {
            ESP_LOGI(TAG, "reply scroll timer stop");
            reply_content_get = false;
            reply_audio_start = false;
            reply_audio_end = false;
            lv_timer_pause(scroll_timer_handle);
            ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, 1000);
        }
    }
}

void ui_ctrl_guide_jump( void )
{
    lv_obj_t *act_scr = lv_scr_act();
    if (act_scr == ui_ScreenSetup) {
        ESP_LOGI(TAG, "act_scr:%p, ui_ScreenSetup:%p", act_scr, ui_ScreenSetup);
        lv_event_send(ui_ButtonSetup, LV_EVENT_CLICKED, 0);
    }
}
