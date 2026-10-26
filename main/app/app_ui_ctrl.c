/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "app_ui_ctrl.h"
#include "app_wifi.h"
#include "app_lang.h"
#include "llm.h"
#include "app_clock.h"
#include "app_sr.h"
#include "tts_stream.h"
#include "settings.h"
#include "assistant.h"
#include "bsp/esp-bsp.h"

#include "ui_helpers.h"
#include "ui.h"

LV_IMG_DECLARE(ui_img_reply_claude);
LV_IMG_DECLARE(ui_img_reply_web);
LV_IMG_DECLARE(ui_img_reply_local);
LV_IMG_DECLARE(ui_img_reply_gemini);
LV_FONT_DECLARE(ui_font_Latin14);
LV_FONT_DECLARE(ui_font_Latin16);
LV_FONT_DECLARE(ui_font_Latin20);

#define LABEL_WIFI_DOT_COUNT_MAX        (10)
#define WIFI_CHECK_TIMER_INTERVAL_S     (1)
/* The answer scrolls with the speech: position refreshed every 100 ms */
#define REPLY_SCROLL_TIMER_INTERVAL_MS  (100)
/* Fraction of the visible height kept above the line being read */
#define REPLY_SCROLL_LEAD_DIV           (3)
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
/* Start of the spoken answer, to follow the reading position */
static int64_t reply_audio_start_us = 0;
/* Text the scrolling animation was computed for (restarted when it grows) */
static size_t s_scroll_chars = 0;
static lv_coord_t s_scroll_height = 0;

static void reply_content_scroll_timer_handler();
static void reply_scroll_stop(void);
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

/* Two taps within this delay on the answer stop it and listen again */
#define DOUBLE_TAP_MS                   (400)

/* Double tap on the answer: same as the wake word (the reply stops, the assistant listens) */
static void reply_double_tap_cb(lv_event_t *e)
{
    static int64_t last_tap_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - last_tap_us <= DOUBLE_TAP_MS * 1000LL) {
        last_tap_us = 0;
        ESP_LOGI(TAG, "double tap: reply stopped, listening");
        app_sr_start_once();
    } else {
        last_tap_us = now;
    }
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
/* Labels of the rows created here, translated when the language changes */
static lv_obj_t *s_model_row_label = NULL;
static lv_obj_t *s_voice_row_label = NULL;
static lv_obj_t *s_save_label = NULL;

static void settings_toast_timer_cb(lv_timer_t *timer)
{
    lv_obj_del((lv_obj_t *)timer->user_data);
    lv_timer_del(timer);
}

/* Show the confirmation over the screen for a moment: the settings apply at once, without restarting */
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

    lv_timer_create(settings_toast_timer_cb, SETTINGS_RESTART_DELAY_MS, overlay);
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
        if (ret == ESP_OK) {
            assistant_request_language(s_settings_languages[lang]);
        }
    }
    // The entry after the listed models is a model set in CONFIG.INI: already the stored one
    uint16_t model = lv_dropdown_get_selected(s_model_dropdown);
    if (ret == ESP_OK && model != s_model_stored && model < llm_model_count) {
        ret = settings_save_str("Chat_model", llm_models[model].id);
        if (ret == ESP_OK) {
            assistant_request_model(llm_models[model].id);
        }
    }
    // Applied by the main task within a second; meanwhile the button is greyed out again
    lv_obj_add_state(s_save_btn, LV_STATE_DISABLED);
    if (ret == ESP_OK) {
        settings_saved();
    }
}

static void settings_dropdown_setup(lv_obj_t *dropdown, const char *options, uint16_t selected,
                                    lv_event_cb_t changed_cb)
{
    lv_dropdown_set_options(dropdown, options);
    lv_dropdown_set_selected(dropdown, selected);
    // Accented names: Latin font for the field and its list (the arrow keeps the default font and its symbol)
    lv_obj_set_style_text_font(dropdown, &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dropdown), &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(dropdown, changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* OpenAI Realtime voices offered (tts_stream.c) */
#define s_settings_voices       tts_voices
#define SETTINGS_VOICE_COUNT    tts_voice_count
static lv_obj_t *s_voice_dropdown = NULL;

/* The voice applies at once (no restart): saved, the synthesis reconnects, then a sample is spoken */
static void settings_voice_changed_cb(lv_event_t *e)
{
    uint16_t selected = lv_dropdown_get_selected(lv_event_get_target(e));
    if (selected >= SETTINGS_VOICE_COUNT) {
        return;     // the voice set in CONFIG.INI (not listed), already in use
    }
    const char *voice = s_settings_voices[selected];
    settings_save_str("Voice", voice);
    sys_param_t *param = settings_get_parameter();
    strlcpy(param->voice, voice, sizeof(param->voice));
    tts_stream_set_voice(voice);

    char name[24];
    strlcpy(name, voice, sizeof(name));
    name[0] = toupper((unsigned char)name[0]);
    char sample[96];
    snprintf(sample, sizeof(sample), app_lang_get()->voice_sample, name);
    tts_stream_begin(NULL);
    tts_stream_say(sample);
    tts_stream_end();
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
    settings_dropdown_setup(ui_DropdownSettingsRegion, "Français\nEnglish", lang_selected, settings_changed_cb);

    // Model row: the listed models, plus the current one if it was set in CONFIG.INI and is not listed
    s_model_dropdown = settings_row_create(lang->settings_model, lv_pct(62));
    s_model_row_label = lv_obj_get_child(lv_obj_get_parent(s_model_dropdown), 0);
    options[0] = '\0';
    uint16_t model_selected = llm_model_count;
    for (size_t i = 0; i < llm_model_count; i++) {
        strlcat(options, i ? "\n" : "", sizeof(options));
        // Numbered as in the voice command "utilise le modèle 3"
        char numbered[56];
        snprintf(numbered, sizeof(numbered), "%u. %s", (unsigned)(i + 1), llm_models[i].label);
        strlcat(options, numbered, sizeof(options));
        if (strcmp(param->chat_model, llm_models[i].id) == 0) {
            model_selected = i;
        }
    }
    if (model_selected == llm_model_count) {
        strlcat(options, "\n", sizeof(options));
        strlcat(options, param->chat_model, sizeof(options));
    }
    s_model_stored = model_selected;
    settings_dropdown_setup(s_model_dropdown, options, model_selected, settings_changed_cb);

    // Voice row: applied immediately, without the Save button
    lv_obj_t *voice_dropdown = settings_row_create(lang->settings_voice, lv_pct(62));
    s_voice_dropdown = voice_dropdown;
    s_voice_row_label = lv_obj_get_child(lv_obj_get_parent(voice_dropdown), 0);
    options[0] = '\0';
    uint16_t voice_selected = SETTINGS_VOICE_COUNT;
    for (size_t i = 0; i < SETTINGS_VOICE_COUNT; i++) {
        char name[24];
        strlcpy(name, s_settings_voices[i], sizeof(name));
        name[0] = toupper((unsigned char)name[0]);
        strlcat(options, i ? "\n" : "", sizeof(options));
        strlcat(options, name, sizeof(options));
        if (strcmp(param->voice, s_settings_voices[i]) == 0) {
            voice_selected = i;
        }
    }
    if (voice_selected == SETTINGS_VOICE_COUNT) {
        strlcat(options, "\n", sizeof(options));
        strlcat(options, param->voice, sizeof(options));
    }
    settings_dropdown_setup(voice_dropdown, options, voice_selected, settings_voice_changed_cb);

    // Save button (bottom right), enabled once a selection differs from the stored settings
    s_save_btn = lv_btn_create(ui_ScreenSettings);
    lv_obj_align(s_save_btn, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_set_style_bg_color(s_save_btn, lv_color_hex(0x6E47BD), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_state(s_save_btn, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_save_btn, settings_save_clicked_cb, LV_EVENT_CLICKED, NULL);
    s_save_label = lv_label_create(s_save_btn);
    lv_label_set_text(s_save_label, lang->settings_save);
    lv_obj_set_style_text_font(s_save_label, &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(s_save_label);
}

/* Texts of the screens, applied at start-up and again when the language changes */
static void ui_ctrl_apply_texts(void)
{
    const app_lang_t *lang = app_lang_get();
    label_localize(ui_LabelSetupWifi, lang->wifi_connecting, &ui_font_Latin20);
    label_localize(ui_LabelSetupStepTile, lang->steps_title, &ui_font_Latin20);
    label_localize(ui_LabelSetupStepContent, lang->steps_content, &ui_font_Latin14);
    label_localize(ui_LabelSettingsTile, lang->settings_title, &ui_font_Latin20);
    label_localize(ui_LabelSettingsRegion, lang->settings_language, &ui_font_Latin16);
    if (s_model_row_label) {
        lv_label_set_text(s_model_row_label, lang->settings_model);
        lv_label_set_text(s_voice_row_label, lang->settings_voice);
        lv_label_set_text(s_save_label, lang->settings_save);
    }
    label_localize(ui_LabelResetTitle, lang->reset_title, &ui_font_Latin20);
    label_localize(ui_LabelResetContent, lang->reset_content, &ui_font_Latin14);
    label_localize(ui_LabelWifiResetTitle, lang->reset_title, &ui_font_Latin20);
    label_localize(ui_LabelWifiResetContent, lang->reset_content, &ui_font_Latin14);
}

void ui_ctrl_settings_sync(void)
{
    bsp_display_lock(0);
    const sys_param_t *param = settings_get_parameter();
    uint16_t lang_selected = 1;   // English UI for other languages
    for (uint16_t i = 0; i < sizeof(s_settings_languages) / sizeof(s_settings_languages[0]); i++) {
        if (strcasecmp(param->lang, s_settings_languages[i]) == 0) {
            lang_selected = i;
        }
    }
    s_lang_stored = lang_selected;
    lv_dropdown_set_selected(ui_DropdownSettingsRegion, lang_selected);
    for (size_t i = 0; i < llm_model_count; i++) {
        if (strcmp(param->chat_model, llm_models[i].id) == 0) {
            s_model_stored = i;
            lv_dropdown_set_selected(s_model_dropdown, i);
        }
    }
    // Voice changed by voice ("change de voix")
    for (size_t i = 0; i < SETTINGS_VOICE_COUNT && s_voice_dropdown; i++) {
        if (strcmp(param->voice, s_settings_voices[i]) == 0) {
            lv_dropdown_set_selected(s_voice_dropdown, i);
        }
    }
    lv_obj_add_state(s_save_btn, LV_STATE_DISABLED);
    bsp_display_unlock();
}

void ui_ctrl_open_settings(void)
{
    // The panels behind are back to sleep, as when the Settings screen is left with its back arrow
    ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, 0);
    bsp_display_lock(0);
    lv_event_send(ui_ImageListenSettings, LV_EVENT_CLICKED, NULL);
    bsp_display_unlock();
}

void ui_ctrl_apply_language(void)
{
    bsp_display_lock(0);
    ui_ctrl_apply_texts();
    app_clock_set_weekdays(app_lang_get()->weekdays);
    bsp_display_unlock();
    ui_ctrl_settings_sync();
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
    settings_screen_setup();
    ui_ctrl_apply_texts();
}

void ui_ctrl_init(void)
{
    bsp_display_lock(0);

    ui_init();
    ui_ctrl_localize();
    app_clock_create(ui_PanelSleep, app_lang_get()->weekdays);
    // Taps on the answer land on the panel or on the scrolling text container (both clickable)
    lv_obj_add_event_cb(ui_PanelReply, reply_double_tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_ContainerReplyContent, reply_double_tap_cb, LV_EVENT_CLICKED, NULL);

    scroll_timer_handle = lv_timer_create(reply_content_scroll_timer_handler, REPLY_SCROLL_TIMER_INTERVAL_MS, NULL);
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
        reply_scroll_stop();
        break;
    case UI_CTRL_PANEL_GET:
        show_panel = ui_PanelGet;
        hide_panel[0] = ui_PanelSleep;
        hide_panel[1] = ui_PanelListen;
        hide_panel[2] = ui_PanelReply;
        lv_obj_clear_flag(ui_LabelListenSpeak, LV_OBJ_FLAG_HIDDEN);
        listen_label_set_text(app_lang_get()->thinking);
        // The next answer starts at the top
        reply_scroll_stop();
        lv_obj_scroll_to_y(ui_ContainerReplyContent, 0, LV_ANIM_OFF);
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

bool ui_ctrl_is_asleep(void)
{
    return current_panel == UI_CTRL_PANEL_SLEEP;
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

    // Refreshed every 200 ms while the answer streams: set as is (the model writes real line breaks)
    lv_label_set_text(ui_LabelReplyContent, text);
    content_height = lv_obj_get_self_height(ui_LabelReplyContent);
    reply_content_get = true;
    lv_timer_resume(scroll_timer_handle);
    ESP_LOGD(TAG, "reply scroll timer start");
}

void ui_ctrl_reply_append_notice(const char *notice)
{
    bsp_display_lock(0);
    char *text = NULL;
    asprintf(&text, "%s\n\n%s", lv_label_get_text(ui_LabelReplyContent), notice);
    if (text) {
        reply_content_show_text(text);
        free(text);
    }
    bsp_display_unlock();
}

void ui_ctrl_label_show_text(ui_ctrl_label_t label, const char *text)
{
    bsp_display_lock(0);

    if (text != NULL) {
        switch (label) {
        case UI_CTRL_LABEL_LISTEN_SPEAK:
            ESP_LOGD(TAG, "update listen speak");
            listen_label_set_text(text);
            break;
        case UI_CTRL_LABEL_REPLY_QUESTION:
            ESP_LOGD(TAG, "update reply question");
            lv_label_set_text(ui_LabelReplyQuestion, text);
            break;
        case UI_CTRL_LABEL_REPLY_CONTENT:
            ESP_LOGD(TAG, "update reply content");
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

void ui_ctrl_reply_set_logo(ui_ctrl_logo_t logo)
{
    const void *src = logo == UI_CTRL_LOGO_WEB ? (const void *)&ui_img_reply_web :
                      logo == UI_CTRL_LOGO_LOCAL ? (const void *)&ui_img_reply_local :
                      logo == UI_CTRL_LOGO_CLAUDE ? (const void *)&ui_img_reply_claude :
                      logo == UI_CTRL_LOGO_GEMINI ? (const void *)&ui_img_reply_gemini :
                      (const void *)&ui_img_reply_chatgpt_logo_png;
    bsp_display_lock(0);
    lv_img_set_src(ui_ImageReplyLogo, src);
    bsp_display_unlock();
}

void ui_ctrl_reply_set_audio_start_flag(bool result)
{
    if (result) {
        reply_audio_start_us = esp_timer_get_time();
    }
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

static void reply_scroll_anim_cb(void *var, int32_t v)
{
    lv_obj_scroll_to_y((lv_obj_t *)var, v, LV_ANIM_OFF);
}

static void reply_scroll_stop(void)
{
    lv_anim_del(ui_ContainerReplyContent, reply_scroll_anim_cb);
    s_scroll_chars = 0;
}

/*
 * Follow the speech: the reading position is estimated from the time since the first audio and the reading
 * speed (characters per second, "Scroll_speed" in CONFIG.INI). The text scrolls with one linear animation
 * (smooth, at the display refresh rate), restarted from where it is when the text grows.
 */
static void reply_scroll_follow(lv_coord_t visible, lv_coord_t max_offset)
{
    size_t chars = 0;
    for (const char *p = lv_label_get_text(ui_LabelReplyContent); *p; p++) {
        chars += ((*p & 0xC0) != 0x80);     // UTF-8: count characters, not bytes
    }
    if (chars == 0 || max_offset <= 0 || (chars == s_scroll_chars && content_height == s_scroll_height)) {
        return;
    }
    s_scroll_chars = chars;
    s_scroll_height = content_height;

    // Speed in pixels per second, and the time (since the first audio) when the text reaches each offset
    float px_per_s = (float)content_height * settings_get_parameter()->scroll_cps / chars;
    float elapsed_s = (esp_timer_get_time() - reply_audio_start_us) / 1000000.0f;
    lv_coord_t offset = lv_obj_get_scroll_y(ui_ContainerReplyContent);
    lv_coord_t lead = visible / REPLY_SCROLL_LEAD_DIV;
    float start_s = (offset + lead) / px_per_s;
    float end_s = (max_offset + lead) / px_per_s;
    if (offset >= max_offset || end_s <= elapsed_s) {
        lv_anim_del(ui_ContainerReplyContent, reply_scroll_anim_cb);
        lv_obj_scroll_to_y(ui_ContainerReplyContent, max_offset, LV_ANIM_OFF);
        return;
    }
    float delay_s = start_s > elapsed_s ? start_s - elapsed_s : 0;
    uint32_t duration_ms = (uint32_t)((end_s - elapsed_s - delay_s) * 1000);

    lv_anim_del(ui_ContainerReplyContent, reply_scroll_anim_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ui_ContainerReplyContent);
    lv_anim_set_exec_cb(&a, reply_scroll_anim_cb);
    lv_anim_set_values(&a, offset, max_offset);
    lv_anim_set_time(&a, duration_ms > 0 ? duration_ms : 1);
    lv_anim_set_delay(&a, (uint32_t)(delay_s * 1000));
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
}

/*
 * Line by line: the text moves up one whole line when the estimated reading position reaches the next
 * line (one redraw per line instead of a continuous movement)
 */
static void reply_scroll_by_line(lv_coord_t visible, lv_coord_t max_offset)
{
    size_t chars = 0;
    for (const char *p = lv_label_get_text(ui_LabelReplyContent); *p; p++) {
        chars += ((*p & 0xC0) != 0x80);     // UTF-8: count characters, not bytes
    }
    lv_coord_t line_h = lv_font_get_line_height(lv_obj_get_style_text_font(ui_LabelReplyContent, LV_PART_MAIN)) +
                        lv_obj_get_style_text_line_space(ui_LabelReplyContent, LV_PART_MAIN);
    if (chars == 0 || max_offset <= 0 || line_h <= 0) {
        return;
    }
    int64_t read_chars = (esp_timer_get_time() - reply_audio_start_us) *
                         settings_get_parameter()->scroll_cps / 1000000;
    if (read_chars > (int64_t)chars) {
        read_chars = chars;
    }
    lv_coord_t reading_y = (lv_coord_t)(content_height * read_chars / (int64_t)chars);
    lv_coord_t lead = visible / REPLY_SCROLL_LEAD_DIV;
    lv_coord_t target = reading_y > lead ? ((reading_y - lead) / line_h) * line_h : 0;
    if (target > max_offset) {
        target = max_offset;
    }
    if (target > lv_obj_get_scroll_y(ui_ContainerReplyContent)) {
        lv_obj_scroll_to_y(ui_ContainerReplyContent, target, LV_ANIM_OFF);
    }
}

static void reply_content_scroll_timer_handler()
{
    if (reply_content_get && reply_audio_start) {
        lv_coord_t visible = lv_obj_get_height(ui_ContainerReplyContent);
        lv_coord_t max_offset = content_height > visible ? content_height - visible : 0;
        if (settings_get_parameter()->scroll_smooth) {
            reply_scroll_follow(visible, max_offset);
        } else {
            reply_scroll_by_line(visible, max_offset);
        }

        if (lv_obj_get_scroll_y(ui_ContainerReplyContent) >= max_offset && reply_audio_end) {
            ESP_LOGI(TAG, "reply scroll timer stop");
            reply_content_get = false;
            reply_audio_start = false;
            reply_audio_end = false;
            lv_timer_pause(scroll_timer_handle);
            reply_scroll_stop();
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
