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
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "app_ui_ctrl.h"
#include "app_wifi.h"
#include "file_lang.h"
#include "llm.h"
#include "app_clock.h"
#include "app_sr.h"
#include "tts_stream.h"
#include "settings.h"
#include "assistant.h"
#include "voice_cmd.h"
#include "model_test.h"
#include "debug_trace.h"
#include "app_audio.h"
#include "ai_providers.h"
#include "file_costs.h"
#include "radio.h"
#include "app_ui_radio.h"
#include "bsp/esp-bsp.h"

#include "ui_helpers.h"
#include "ui.h"

LV_IMG_DECLARE(ui_img_reply_claude);
LV_IMG_DECLARE(ui_img_reply_web);
LV_IMG_DECLARE(ui_img_reply_local);
LV_IMG_DECLARE(ui_img_reply_gemini);
LV_IMG_DECLARE(ui_img_reply_openrouter);
LV_IMG_DECLARE(ui_img_reply_xai);
LV_IMG_DECLARE(ui_img_reply_mistral);
LV_IMG_DECLARE(ui_img_reply_perplexity);
LV_FONT_DECLARE(ui_font_Latin14);
LV_FONT_DECLARE(ui_font_Latin16);
LV_FONT_DECLARE(ui_font_Latin20);

#define LABEL_WIFI_DOT_COUNT_MAX        (10)
#define WIFI_CHECK_TIMER_INTERVAL_S     (1)
/* The answer scrolls with the speech: position refreshed every 100 ms */
#define REPLY_SCROLL_TIMER_INTERVAL_MS  (100)
/* Fraction of the visible height kept above the line being read */
#define REPLY_SCROLL_LEAD_DIV           (3)
/* A list shown, not read: scrolled with the finger; without follow-up listening, back to sleep after this long
 * without a move of the text */
#define REPLY_MANUAL_IDLE_MS            (20000)
/* Model in use under the robot (listening / thinking): provider icon (20 x 21) and model name, at the bottom left */
#define MODEL_ROW_LEFT_X                (8)
#define MODEL_ROW_BOTTOM_Y              (-2)
#define MODEL_ROW_HEIGHT                (21)
#define MODEL_ROW_GAP_X                 (6)
#define MODEL_ROW_COLOR                 0xB9CCF5
/* Provider icon on the robot's forehead: gap above the screen of the eyes (60 x 34), which it follows when the
 * robot looks aside while listening */
#define HEAD_LOGO_GAP_Y                 (25)
/* and its shift to the right of their centre (negative: to the left) */
#define HEAD_LOGO_X                     (-5)
/* Text under the robot (listening / thinking): lines shown, bottom position (from the screen bottom), right above
 * the model in use */
#define LISTEN_TEXT_LINES               (2)
#define LISTEN_TEXT_BOTTOM_Y            (MODEL_ROW_BOTTOM_Y - MODEL_ROW_HEIGHT)
/* The robot is raised to leave room for these lines */
#define ROBOT_RAISE_PX                  (10)

static char *TAG = "ui_ctrl";

static ui_ctrl_panel_t current_panel = UI_CTRL_PANEL_SLEEP;
static lv_timer_t *scroll_timer_handle = NULL;
static bool reply_audio_start = false;
static bool reply_audio_end = false;
static bool reply_content_get = false;
static uint16_t content_height = 0;
/* Start of the spoken answer, to follow the reading position */
static int64_t reply_audio_start_us = 0;
/* The reply shows a list not read: not scrolled with the speech, but with the finger (arrow while more is below) */
static bool s_manual_scroll = false;
static int64_t s_manual_idle_us = 0;
static void scroll_arrow_update(void);
/* Text the scrolling animation was computed for (restarted when it grows) */
static size_t s_scroll_chars = 0;
static lv_coord_t s_scroll_height = 0;

static void reply_content_scroll_timer_handler();
static void reply_scroll_stop(void);
static void wifi_check_timer_handler(lv_timer_t *timer);
static void setup_button_setup(void);

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

/* Double tap while listening ("J'écoute") or thinking ("Je réfléchis"): dropped, back to sleep */
static void cancel_double_tap_cb(lv_event_t *e)
{
    static int64_t last_tap_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - last_tap_us <= DOUBLE_TAP_MS * 1000LL) {
        last_tap_us = 0;
        assistant_cancel();
    } else {
        last_tap_us = now;
    }
}

/*
 * Double tap on the answer: same as the wake word (the reply stops, the assistant listens); on a question of a
 * voice command (language, model...): the command is dropped, back to sleep
 */
/* An item tapped is highlighted this long before it is taken as the answer */
#define ITEM_TAP_HIGHLIGHT_MS   250
#define ITEM_TAP_COLOR          0x4FC3F7
static lv_obj_t *s_item_highlight = NULL;
static bool s_item_tap_pending = false;

/* Over the lines of the item tapped (text from item to item_end), on the reply panel */
static void item_highlight_show(const char *text, const char *item, const char *item_end)
{
    if (!s_item_highlight) {
        s_item_highlight = lv_obj_create(ui_PanelReply);
        lv_obj_remove_style_all(s_item_highlight);
        lv_obj_set_style_radius(s_item_highlight, 4, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_item_highlight, lv_color_hex(ITEM_TAP_COLOR), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_item_highlight, LV_OPA_40, LV_PART_MAIN);
        lv_obj_clear_flag(s_item_highlight, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }
    lv_point_t first, last;
    lv_label_get_letter_pos(ui_LabelReplyContent, _lv_txt_encoded_get_char_id(text, item - text), &first);
    lv_label_get_letter_pos(ui_LabelReplyContent,
                            _lv_txt_encoded_get_char_id(text, (item_end > item ? item_end - 1 : item) - text), &last);
    lv_coord_t line_h = lv_font_get_line_height(lv_obj_get_style_text_font(ui_LabelReplyContent, LV_PART_MAIN));
    lv_area_t label, box, panel;
    lv_obj_get_coords(ui_LabelReplyContent, &label);
    lv_obj_get_coords(ui_ContainerReplyContent, &box);
    lv_obj_get_coords(ui_PanelReply, &panel);
    // Within the visible part of the text
    lv_coord_t y1 = LV_MAX(label.y1 + first.y - 2, box.y1);
    lv_coord_t y2 = LV_MIN(label.y1 + last.y + line_h + 1, box.y2);
    if (y2 <= y1) {
        return;
    }
    lv_obj_set_pos(s_item_highlight, box.x1 - panel.x1 - 4, y1 - panel.y1);
    lv_obj_set_size(s_item_highlight, lv_area_get_width(&box) + 8, y2 - y1);
    lv_obj_move_foreground(s_item_highlight);
    lv_obj_clear_flag(s_item_highlight, LV_OBJ_FLAG_HIDDEN);
}

/* The highlight seen: the item is the answer */
static void item_tap_timer_cb(lv_timer_t *timer)
{
    s_item_tap_pending = false;
    if (s_item_highlight) {
        lv_obj_add_flag(s_item_highlight, LV_OBJ_FLAG_HIDDEN);
    }
    app_sr_answer_tapped((int)(intptr_t)timer->user_data);
}

/*
 * The number of the item of a list under the finger (a line "3, Grok (1,2 s)." of the reply), 0 if none; its text
 * (up to the next line break) in *item, *item_end
 */
static int reply_item_tapped(const char **item, const char **item_end)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) {
        return 0;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t area;
    lv_obj_get_coords(ui_LabelReplyContent, &area);
    if (p.y < area.y1 || p.y > area.y2) {
        return 0;
    }
    lv_point_t rel = { .x = p.x > area.x1 ? p.x - area.x1 : 0, .y = p.y - area.y1 };
    const char *text = lv_label_get_text(ui_LabelReplyContent);
    uint32_t byte = _lv_txt_encoded_get_byte_id(text, lv_label_get_letter_on(ui_LabelReplyContent, &rel));
    size_t len = strlen(text);
    const char *line = text + (byte < len ? byte : len);
    while (line > text && line[-1] != '\n') {
        line--;
    }
    if (line == text) {
        return 0;       // the first line: the question, not an item
    }
    *item = line;
    *item_end = strchr(line, '\n');
    if (!*item_end) {
        *item_end = text + len;
    }
    int number = 0;
    while (isdigit((unsigned char)*line) && number < 1000) {
        number = number * 10 + (*line++ - '0');
    }
    return *line == ',' ? number : 0;
}

static void reply_double_tap_cb(lv_event_t *e)
{
    static int64_t last_tap_us = 0;
    int64_t now = esp_timer_get_time();
    // A question of a voice command waits for its answer: a tap on an item of its list answers it (highlighted
    // a moment first, to see it was tapped)
    if (s_item_tap_pending) {
        return;
    }
    const char *item_text = NULL, *item_end = NULL;
    int item = voice_cmd_waiting_answer() ? reply_item_tapped(&item_text, &item_end) : 0;
    if (item > 0) {
        last_tap_us = 0;
        ESP_LOGI(TAG, "tap on item %d of the list", item);
        item_highlight_show(lv_label_get_text(ui_LabelReplyContent), item_text, item_end);
        s_item_tap_pending = true;
        lv_timer_t *timer = lv_timer_create(item_tap_timer_cb, ITEM_TAP_HIGHLIGHT_MS, (void *)(intptr_t)item);
        lv_timer_set_repeat_count(timer, 1);
        return;
    }
    if (now - last_tap_us <= DOUBLE_TAP_MS * 1000LL) {
        last_tap_us = 0;
        if (voice_cmd_waiting_answer()) {
            ESP_LOGI(TAG, "double tap: question of the voice command dropped");
            assistant_cancel();
            return;
        }
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
#define SETTINGS_OPTIONS_SIZE           1024    /* up to 22 models */


static lv_obj_t *s_model_dropdown = NULL;
static void head_icons_refresh(void);

/* Position of a language in the dropdown (Languages of CONFIG.INI); English, or the first, when not offered */
static uint16_t settings_language_index(const char *code)
{
    uint16_t found = 0;
    for (size_t i = 0; i < app_lang_count(); i++) {
        if (strcasecmp(code, app_lang_code(i)) == 0) {
            return (uint16_t)i;
        }
        found = strcasecmp(app_lang_code(i), "en") == 0 ? (uint16_t)i : found;
    }
    return found;
}
static lv_obj_t *s_save_btn = NULL;
static int s_settings_page = 0;
/* The Confirm button: shown only when the language or the model was changed, on their page (the first) */
static bool s_settings_changed = false;

static void confirm_button_update(bool changed)
{
    s_settings_changed = changed;
    if (!s_save_btn) {
        return;
    }
    if (changed && s_settings_page == 0) {
        lv_obj_clear_flag(s_save_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_save_btn, LV_OBJ_FLAG_HIDDEN);
    }
}
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

/* A text over the screen for a moment (under the display lock) */
static void toast_show(const char *text)
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
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, 280);
    lv_obj_set_style_text_font(label, &ui_font_Latin20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(label);

    lv_timer_create(settings_toast_timer_cb, SETTINGS_RESTART_DELAY_MS, overlay);
}

void ui_ctrl_toast(const char *text)
{
    bsp_display_lock(0);
    toast_show(text);
    bsp_display_unlock();
}

/* Show the confirmation over the screen for a moment: the settings apply at once, without restarting */
static void settings_saved(void)
{
    toast_show(app_lang_get()->settings_saved);
}

/* With the debug traces on: a long press on the assistant's screen puts a mark in them (and says its number) */
static void mark_long_pressed_cb(lv_event_t *e)
{
    static int64_t last_us = 0;
    int64_t now = esp_timer_get_time();
    // Once per press (the text of the reply and its panel both get it)
    if (!debug_trace_on() || now - last_us < 1000 * 1000) {
        return;
    }
    last_us = now;
    char text[64];
    snprintf(text, sizeof(text), app_lang_get()->mark_done, debug_trace_mark("long press"));
    toast_show(text);
}

/* Enable the Save button when the selection differs from the stored settings */
static void settings_changed_cb(lv_event_t *e)
{
    bool changed = lv_dropdown_get_selected(ui_DropdownSettingsRegion) != s_lang_stored ||
                   lv_dropdown_get_selected(s_model_dropdown) != s_model_stored;
    confirm_button_update(changed);
}

static void settings_save_clicked_cb(lv_event_t *e)
{
    esp_err_t ret = ESP_OK;
    uint16_t lang = lv_dropdown_get_selected(ui_DropdownSettingsRegion);
    if (lang != s_lang_stored && lang < app_lang_count()) {
        ret = settings_save_str("Language", app_lang_code(lang));
        if (ret == ESP_OK) {
            assistant_request_language(app_lang_code(lang));
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
    // Applied by the main task within a second; meanwhile the button is hidden again
    confirm_button_update(false);
    if (ret == ESP_OK) {
        settings_saved();
    }
}

static void settings_dropdown_setup(lv_obj_t *dropdown, const char *options, uint16_t selected,
                                    lv_event_cb_t changed_cb, void *user_data)
{
    lv_dropdown_set_options(dropdown, options);
    lv_dropdown_set_selected(dropdown, selected);
    // Accented names: Latin font for the field and its list
    lv_obj_set_style_text_font(dropdown, &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dropdown), &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(dropdown, changed_cb, LV_EVENT_VALUE_CHANGED, user_data);
}

/* Voices offered: those of [voices] (AGENTS.INI) of the speech provider in use that speak the language in use */
#define SETTINGS_VOICES_MAX     32
static const llm_voice_t *s_settings_voices[SETTINGS_VOICES_MAX];
static size_t s_settings_voice_count = 0;
static lv_obj_t *s_voice_dropdown = NULL;

/* The options of the voice row (again when the speech provider or the language changes), the voice in use selected */
static void voice_options_refresh(void)
{
    if (!s_voice_dropdown) {
        return;
    }
    const char *provider = assistant_tts_provider();
    const char *code = app_lang_get()->code;
    const llm_voice_t *current = assistant_voice();
    char options[SETTINGS_VOICES_MAX * 25] = "";
    uint16_t selected = 0;
    s_settings_voice_count = 0;
    for (size_t i = 0; i < llm_voice_count() && s_settings_voice_count < SETTINGS_VOICES_MAX; i++) {
        const llm_voice_t *v = llm_voice(i);
        if (!llm_voice_offered(v, provider, code)) {
            continue;
        }
        if (v == current) {
            selected = s_settings_voice_count;
        }
        strlcat(options, s_settings_voice_count ? "\n" : "", sizeof(options));
        strlcat(options, v->name, sizeof(options));
        s_settings_voices[s_settings_voice_count++] = v;
    }
    lv_dropdown_set_options(s_voice_dropdown, options[0] ? options : "-");
    lv_dropdown_set_selected(s_voice_dropdown, selected);
}

/* The voice applies at once (no restart): saved, the synthesis reconnects, then a sample is spoken */
static void settings_voice_changed_cb(lv_event_t *e)
{
    uint16_t selected = lv_dropdown_get_selected(lv_event_get_target(e));
    if (selected >= s_settings_voice_count) {
        return;
    }
    const llm_voice_t *voice = s_settings_voices[selected];
    settings_save_str("Voice", voice->name);
    sys_param_t *param = settings_get_parameter();
    strlcpy(param->voice, voice->name, sizeof(param->voice));
    assistant_set_tts(NULL);

    char sample[96];
    snprintf(sample, sizeof(sample), app_lang_get()->voice_sample, voice->name);
    tts_stream_begin(NULL);
    tts_stream_say(sample);
    tts_stream_end();
}

/* Transcription and speech models offered: those of [stt] / [tts] (AGENTS.INI) whose provider has a key */
#define SETTINGS_SPEECH_MAX     16
typedef struct {
    llm_speech_kind_t kind;
    lv_obj_t *dropdown;
    lv_obj_t *label;
    const llm_speech_model_t *models[SETTINGS_SPEECH_MAX];
    size_t count;
} settings_speech_t;
static settings_speech_t s_settings_speech[2] = {
    { .kind = LLM_SPEECH_STT },
    { .kind = LLM_SPEECH_TTS },
};

/* The options of a speech row, the model in use selected (added at the end when it is not listed) */
static void speech_options_refresh(settings_speech_t *row)
{
    if (!row->dropdown) {
        return;
    }
    const sys_param_t *param = settings_get_parameter();
    const char *current = row->kind == LLM_SPEECH_STT ? param->stt_model : param->tts_model;
    char options[SETTINGS_SPEECH_MAX * 40] = "";
    uint16_t selected = SETTINGS_SPEECH_MAX;
    row->count = 0;
    for (size_t i = 0; i < llm_speech_count(row->kind) && row->count < SETTINGS_SPEECH_MAX; i++) {
        const llm_speech_model_t *m = llm_speech_model(row->kind, i);
        if (!assistant_speech_key(m->provider)[0]) {
            continue;       // its provider has no key (Mistral_key, Deepgram_key)
        }
        if (strcmp(m->id, current) == 0) {
            selected = row->count;
        }
        strlcat(options, row->count ? "\n" : "", sizeof(options));
        strlcat(options, m->label, sizeof(options));
        row->models[row->count++] = m;
    }
    if (selected == SETTINGS_SPEECH_MAX) {
        selected = row->count;
        strlcat(options, row->count ? "\n" : "", sizeof(options));
        strlcat(options, current[0] ? current : "-", sizeof(options));
    }
    lv_dropdown_set_options(row->dropdown, options);
    lv_dropdown_set_selected(row->dropdown, selected);
}

/* A speech model applies at once (no restart), as with "change de synthèse vocale": saved, then reconnected */
static void settings_speech_changed_cb(lv_event_t *e)
{
    settings_speech_t *row = lv_event_get_user_data(e);
    uint16_t selected = lv_dropdown_get_selected(row->dropdown);
    if (selected >= row->count) {
        return;
    }
    const llm_speech_model_t *m = row->models[selected];
    if (settings_save_str(row->kind == LLM_SPEECH_STT ? "Stt_model" : "Tts_model", m->id) != ESP_OK) {
        return;
    }
    if (row->kind == LLM_SPEECH_STT) {
        assistant_set_stt(m->id);
        return;
    }
    assistant_set_tts(m->id);
    // The voices of its provider, and a sample spoken with the one now in use
    voice_options_refresh();
    const llm_voice_t *voice = assistant_voice();
    if (voice) {
        char sample[96];
        snprintf(sample, sizeof(sample), app_lang_get()->voice_sample, voice->name);
        tts_stream_begin(NULL);
        tts_stream_say(sample);
        tts_stream_end();
    }
}

/* The models, numbered as in the voice command "utilise le modèle 3"; returns the index of the current one */
static uint16_t model_options(char *options, size_t size)
{
    const sys_param_t *param = settings_get_parameter();
    options[0] = '\0';
    uint16_t model_selected = llm_model_count;
    for (size_t i = 0; i < llm_model_count; i++) {
        strlcat(options, i ? "\n" : "", size);
        char numbered[72];
        snprintf(numbered, sizeof(numbered), "%u. %s", (unsigned)(i + 1), llm_models[i].label);
        strlcat(options, numbered, size);
        if (strcmp(param->chat_model, llm_models[i].id) == 0) {
            model_selected = i;
        }
    }
    // The current model if it was set in CONFIG.INI and is not listed
    if (model_selected == llm_model_count) {
        strlcat(options, "\n", size);
        strlcat(options, param->chat_model, size);
    }
    return model_selected;
}

/* Text of a dropdown list being made (under the display lock): PSRAM, internal RAM is scarce */
static char *dropdown_options(void)
{
    static char *options = NULL;
    if (!options) {
        options = heap_caps_malloc(SETTINGS_OPTIONS_SIZE, MALLOC_CAP_SPIRAM);
        assert(options);
    }
    options[0] = '\0';
    return options;
}

/* The models offered in the model row (under the display lock) */
static void model_dropdown_refresh(void)
{
    if (s_model_dropdown) {
        char *options = dropdown_options();
        uint16_t selected = model_options(options, SETTINGS_OPTIONS_SIZE);
        lv_dropdown_set_options(s_model_dropdown, options);
        lv_dropdown_set_selected(s_model_dropdown, selected);
        s_model_stored = selected;
        confirm_button_update(false);
    }
}

void ui_ctrl_models_changed(void)
{
    bsp_display_lock(0);
    model_dropdown_refresh();
    bsp_display_unlock();
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
    // No arrow: the font has no glyph for it, it showed as a rectangle
    lv_dropdown_set_symbol(dropdown, NULL);
    return dropdown;
}

/*
 * Pages of the Settings screen, browsed with the arrows at the bottom (looping): 1. language and model,
 * 2. speech, transcription and voice, 3. to 5. the LLM, STT and TTS models, 6. the debug traces. The rows of the other
 * pages are hidden
 * (the panel's flex layout skips them).
 */
#define SETTINGS_PAGES          6
#define SETTINGS_PAGE_MODELS    2       /* the first page of the models (LLM, then STT, then TTS) */
#define SETTINGS_PAGE_DEBUG     5
#define SETTINGS_ROWS_MAX       12
static void model_list_show(int page);
static void debug_page_refresh(void);
static lv_obj_t *s_settings_rows[SETTINGS_ROWS_MAX];
static uint8_t s_settings_row_page[SETTINGS_ROWS_MAX];
static size_t s_settings_row_count = 0;

static void settings_row_add(lv_obj_t *row, uint8_t page)
{
    assert(s_settings_row_count < SETTINGS_ROWS_MAX);
    s_settings_rows[s_settings_row_count] = row;
    s_settings_row_page[s_settings_row_count++] = page;
}

/* The language and the model chosen but not confirmed: dropped, the stored ones shown again */
static void settings_unconfirmed_drop(void)
{
    if (!s_save_btn || !s_settings_changed) {
        return;
    }
    ESP_LOGI(TAG, "language / model not confirmed: dropped");
    lv_dropdown_set_selected(ui_DropdownSettingsRegion, s_lang_stored);
    lv_dropdown_set_selected(s_model_dropdown, s_model_stored);
    confirm_button_update(false);
}

/* A page shown; the language and the model not confirmed are dropped, the Confirm button only on their page */
static void settings_page_show(int page)
{
    settings_unconfirmed_drop();
    s_settings_page = (page + SETTINGS_PAGES) % SETTINGS_PAGES;
    confirm_button_update(s_settings_changed);
    model_list_show(s_settings_page);
    debug_page_refresh();
    for (size_t i = 0; i < s_settings_row_count; i++) {
        if (s_settings_row_page[i] == s_settings_page) {
            lv_obj_clear_flag(s_settings_rows[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_settings_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void settings_page_clicked_cb(lv_event_t *e)
{
    settings_page_show(s_settings_page + (int)(intptr_t)lv_event_get_user_data(e));
}

/* Back to the first page each time the screen is shown */
static void settings_screen_loaded_cb(lv_event_t *e)
{
    settings_page_show(0);
}

/* The screen left without confirming: the language and the model chosen are dropped */
static void settings_screen_unload_cb(lv_event_t *e)
{
    settings_unconfirmed_drop();
}

/* A button at the bottom of the Settings screen (arrows, Save) */
static lv_obj_t *settings_button_create(lv_align_t align, lv_coord_t x)
{
    lv_obj_t *btn = lv_btn_create(ui_ScreenSettings);
    lv_obj_align(btn, align, x, -10);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x6E47BD), LV_PART_MAIN | LV_STATE_DEFAULT);
    return btn;
}

static void settings_arrow_create(lv_align_t align, lv_coord_t x, const char *symbol, int step)
{
    lv_obj_t *btn = settings_button_create(align, x);
    lv_obj_set_width(btn, 50);
    lv_obj_add_event_cb(btn, settings_page_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)step);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    // The default font has the arrow symbols (the Latin ones do not)
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(label);
}

/*
 * ---------- Pages of the models: those of AGENTS.INI (LLM: then the free OpenRouter ones of [openrouter_free]) ----------
 * A check box on the left enables one (a "-" before its id when not: offered nowhere, not tested by "teste les
 * modèles"); the one in use cannot be unchecked. Then its provider and name (grey: no key for it), and the time to
 * the first words of its last test. The list scrolls; a long press asks whether to test that model alone.
 */
#define MODEL_ROWS_MAX      80
#define MODEL_ROW_DIM       0x8A97BC
#define MODEL_ROW_NO_KEY    0x5A6280

typedef struct {
    char id[80];
    char name[48];
    lv_obj_t *check;
    lv_obj_t *time;
    model_test_kind_t kind;
    bool free;              /* a line of [openrouter_free] */
} model_row_t;

static lv_obj_t *s_model_lists[3];          /* LLM, STT, TTS: scrollable, filled when their page is shown */
static model_row_t *s_model_rows = NULL;    /* of the list shown (PSRAM) */
static size_t s_model_row_count = 0;
static lv_obj_t *s_model_test_box = NULL;
static int s_model_test_row = -1;
static lv_obj_t *s_model_test_wait = NULL;

/*
 * While a model is tested alone: over the whole screen (nothing can be touched), the robot's head looking from
 * side to side in a turning ring, and a text; removed when the test is done
 */
static void model_test_eyes_anim_cb(void *eyes, int32_t x)
{
    lv_obj_set_x(eyes, x);
}

static void busy_show(const char *text)
{
    if (s_model_test_wait) {
        return;
    }
    s_model_test_wait = lv_obj_create(lv_layer_top());
    lv_obj_t *wait = s_model_test_wait;
    lv_obj_remove_style_all(wait);
    lv_obj_set_size(wait, lv_pct(100), lv_pct(100));
    lv_obj_add_flag(wait, LV_OBJ_FLAG_CLICKABLE);     // the touches stop here
    lv_obj_clear_flag(wait, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(wait, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(wait, LV_OPA_80, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *ring = lv_spinner_create(wait, 1200, 70);
    lv_obj_set_size(ring, 156, 156);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, -18);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(ring, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_color(ring, lv_color_hex(0x2A2F45), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_width(ring, 6, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_color(ring, lv_color_hex(0x6E47BD), LV_PART_INDICATOR | LV_STATE_DEFAULT);

    // The head as on the listening screen: body, eye screen, eyes
    lv_obj_t *body = lv_img_create(wait);
    lv_img_set_src(body, &ui_img_body_png);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, -18);
    lv_obj_t *screen = lv_img_create(body);
    lv_img_set_src(screen, &ui_img_body_eye_screen_png);
    lv_obj_align(screen, LV_ALIGN_CENTER, 10, 5);
    lv_obj_t *eyes = lv_img_create(screen);
    lv_img_set_src(eyes, &ui_img_listen_body_eyes_1_png);
    lv_obj_align(eyes, LV_ALIGN_CENTER, 0, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, eyes);
    lv_anim_set_exec_cb(&a, model_test_eyes_anim_cb);
    lv_anim_set_values(&a, -12, 12);
    lv_anim_set_time(&a, 900);
    lv_anim_set_playback_time(&a, 900);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    lv_obj_t *label = lv_label_create(wait);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, 300);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(label, &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -14);
}

/* Its last test without the parentheses: "1,2 s", "indisponible", "" when never tested */
static void model_row_time(const cJSON *results, const char *key, char *buf, size_t size)
{
    char text[40];
    model_test_result(results, key, text, sizeof(text));
    const char *from = strncmp(text, " (", 2) == 0 ? text + 2 : text;
    strlcpy(buf, from, size);
    size_t n = strlen(buf);
    if (n && buf[n - 1] == ')') {
        buf[n - 1] = '\0';
    }
}

/* The free OpenRouter models chosen again by the main task, the model row with them; the box left as it is */
static void model_check_cb(lv_event_t *e)
{
    model_row_t *r = &s_model_rows[(intptr_t)lv_event_get_user_data(e)];
    bool on = lv_obj_has_state(r->check, LV_STATE_CHECKED);
    bool done = false;
    if (!model_test_running()) {
        if (r->free) {
            done = llm_set_free_enabled(r->id, on);
            if (done) {
                model_test_free_reload();
            }
        } else if (r->kind == MODEL_TEST_KIND_LLM) {
            done = llm_set_model_enabled(r->id, on);
            if (done) {
                model_dropdown_refresh();
            }
        } else {
            settings_speech_t *row = &s_settings_speech[r->kind == MODEL_TEST_KIND_STT ? 0 : 1];
            done = llm_set_speech_enabled(row->kind, r->id, on);
            if (done) {
                speech_options_refresh(row);
            }
        }
    }
    ESP_LOGI(TAG, "%s %s%s", r->id, on ? "enabled" : "disabled", done ? "" : ": refused");
    if (!done) {
        // Not written (a test running, AGENTS.INI not rewritten): as it was
        if (on) {
            lv_obj_clear_state(r->check, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(r->check, LV_STATE_CHECKED);
        }
    }
}

static void busy_hide(void)
{
    if (s_model_test_wait) {
        lv_obj_del(s_model_test_wait);      // its animations with it
        s_model_test_wait = NULL;
    }
}

/* The test done (main task): the list shown again with its time */
static void model_test_done_cb(void)
{
    bsp_display_lock(0);
    s_model_test_row = -1;
    busy_hide();
    if (lv_scr_act() == ui_ScreenSettings) {
        model_list_show(s_settings_page);
    }
    bsp_display_unlock();
}

static void model_test_box_cb(lv_event_t *e)
{
    lv_obj_t *box = lv_event_get_current_target(e);
    bool yes = lv_msgbox_get_active_btn(box) == 0;
    int index = s_model_test_row;
    if (yes && index >= 0 && index < (int)s_model_row_count &&
            model_test_one(s_model_rows[index].kind, s_model_rows[index].id, model_test_done_cb)) {
        ESP_LOGI(TAG, "%s: tested alone", s_model_rows[index].id);
        lv_label_set_text(s_model_rows[index].time, "...");
        char text[96];
        snprintf(text, sizeof(text), app_lang_get()->settings_testing, s_model_rows[index].name);
        busy_show(text);
    } else {
        s_model_test_row = -1;
    }
    s_model_test_box = NULL;
    lv_msgbox_close_async(box);
}

/* A long press on a model: "Test performance", yes or no */
static void model_long_pressed_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (model_test_running() || s_model_test_box || index >= (int)s_model_row_count) {
        return;
    }
    const app_lang_t *lang = app_lang_get();
    static const char *buttons[3];
    buttons[0] = lang->settings_yes;
    buttons[1] = lang->settings_no;
    buttons[2] = "";
    char text[96];
    snprintf(text, sizeof(text), lang->settings_test_text, s_model_rows[index].name);
    s_model_test_row = index;
    s_model_test_box = lv_msgbox_create(NULL, lang->settings_test_title, text, buttons, false);
    lv_obj_set_width(s_model_test_box, 280);
    lv_obj_set_style_text_font(lv_msgbox_get_title(s_model_test_box), &ui_font_Latin16, LV_PART_MAIN);
    lv_obj_set_style_text_font(lv_msgbox_get_text(s_model_test_box), &ui_font_Latin16, LV_PART_MAIN);
    lv_obj_set_style_text_font(lv_msgbox_get_btns(s_model_test_box), &ui_font_Latin16, LV_PART_ITEMS);
    lv_obj_center(s_model_test_box);
    lv_obj_add_event_cb(s_model_test_box, model_test_box_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void model_row_add(lv_obj_t *list, model_test_kind_t kind, const char *id, const char *provider,
                          const char *name, bool enabled, bool current, bool has_key, const cJSON *results, bool free)
{
    if (!s_model_rows || s_model_row_count >= MODEL_ROWS_MAX) {
        return;
    }
    size_t index = s_model_row_count++;
    model_row_t *r = &s_model_rows[index];
    strlcpy(r->id, id, sizeof(r->id));
    strlcpy(r->name, name, sizeof(r->name));
    r->kind = kind;
    r->free = free;

    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 30);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, model_long_pressed_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)index);

    r->check = lv_checkbox_create(row);
    lv_checkbox_set_text(r->check, "");
    // The check mark is a symbol: of the default font (the Latin ones have none)
    lv_obj_set_style_text_font(r->check, &lv_font_montserrat_14, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (enabled) {
        lv_obj_add_state(r->check, LV_STATE_CHECKED);
    }
    if (current) {
        lv_obj_add_state(r->check, LV_STATE_DISABLED);      // the one in use stays enabled
    }
    lv_obj_add_event_cb(r->check, model_check_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)index);
    lv_obj_add_event_cb(r->check, model_long_pressed_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)index);

    lv_obj_t *label = lv_label_create(row);
    char text[128];
    snprintf(text, sizeof(text), "#%06X %s# %s", MODEL_ROW_DIM, provider, name);
    lv_label_set_recolor(label, true);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_style_text_font(label, &ui_font_Latin14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, lv_color_hex(has_key ? 0xFFFFFF : MODEL_ROW_NO_KEY),
                                LV_PART_MAIN | LV_STATE_DEFAULT);

    r->time = lv_label_create(row);
    // Kept by its id, a speech model by "stt:<id>" / "tts:<id>"
    char key[96], time_text[32];
    snprintf(key, sizeof(key), "%s%s", kind == MODEL_TEST_KIND_LLM ? "" : kind == MODEL_TEST_KIND_STT ? "stt:" : "tts:",
             id);
    model_row_time(results, key, time_text, sizeof(time_text));
    lv_label_set_text(r->time, s_model_test_row == (int)index && model_test_running() ? "..." : time_text);
    lv_obj_set_style_text_font(r->time, &ui_font_Latin14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(r->time, lv_color_hex(MODEL_ROW_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
}

static int compare_order(const void *a, const void *b)
{
    return (*(const llm_model_t * const *)a)->order - (*(const llm_model_t * const *)b)->order;
}

/* The chat models of AGENTS.INI in its order (enabled or not), then the free OpenRouter ones */
static void model_list_llm(lv_obj_t *list, const cJSON *results)
{
    const char *current = settings_get_parameter()->chat_model;
    size_t count = 0;
    const llm_model_t **models = heap_caps_calloc(llm_model_count + llm_disabled_count() + 1, sizeof(*models),
                                                  MALLOC_CAP_SPIRAM);
    if (!models) {
        return;
    }
    for (size_t i = 0; i < llm_model_count; i++) {
        if (llm_models[i].order >= 0) {
            models[count++] = &llm_models[i];
        }
    }
    for (size_t i = 0; i < llm_disabled_count(); i++) {
        models[count++] = llm_disabled_model(i);
    }
    qsort(models, count, sizeof(*models), compare_order);
    for (size_t i = 0; i < count; i++) {
        const ai_provider_t *p = ai_provider_of_llm(models[i]->provider);
        bool enabled = models[i] >= llm_models && models[i] < llm_models + llm_model_count;
        model_row_add(list, MODEL_TEST_KIND_LLM, models[i]->id, p->company, models[i]->label, enabled,
                      strcmp(models[i]->id, current) == 0, ai_provider_key(p)[0], results, false);
    }
    free(models);
    const ai_provider_t *openrouter = ai_provider_of_llm(LLM_PROVIDER_OPENROUTER);
    const char *name, *id;
    bool enabled;
    for (size_t i = 0; (id = llm_free_line(i, &name, &enabled)) != NULL; i++) {
        model_row_add(list, MODEL_TEST_KIND_LLM, id, openrouter->company, name, enabled, strcmp(id, current) == 0,
                      ai_provider_key(openrouter)[0], results, true);
    }
}

/* The transcription or speech models of [stt] / [tts], enabled or not */
static void model_list_speech(lv_obj_t *list, model_test_kind_t kind, const cJSON *results)
{
    llm_speech_kind_t speech = kind == MODEL_TEST_KIND_STT ? LLM_SPEECH_STT : LLM_SPEECH_TTS;
    const sys_param_t *param = settings_get_parameter();
    const char *current = speech == LLM_SPEECH_STT ? param->stt_model : param->tts_model;
    bool enabled;
    for (size_t i = 0; i < llm_speech_all_count(speech); i++) {
        const llm_speech_model_t *m = llm_speech_all(speech, i, &enabled);
        const ai_provider_t *p = ai_provider_find(m->provider);
        model_row_add(list, kind, m->id, p ? p->company : m->provider, m->label, enabled,
                      strcmp(m->id, current) == 0, assistant_speech_key(m->provider)[0], results, false);
    }
}

/* The title of the page; a page of models filled again (the others emptied: their rows are many) */
static void model_list_show(int page)
{
    const app_lang_t *lang = app_lang_get();
    const char *titles[3] = { lang->settings_llm_list, lang->settings_stt_list, lang->settings_tts_list };
    int list = page - SETTINGS_PAGE_MODELS;
    if (ui_LabelSettingsTile) {
        lv_label_set_text(ui_LabelSettingsTile, list >= 0 && list < 3 ? titles[list] :
                          page == SETTINGS_PAGE_DEBUG ? lang->settings_debug_title : lang->settings_title);
    }
    for (int i = 0; i < 3; i++) {
        if (s_model_lists[i]) {
            lv_obj_clean(s_model_lists[i]);
        }
    }
    s_model_row_count = 0;
    if (list < 0 || list >= 3 || !s_model_lists[list]) {
        return;
    }
    if (!s_model_rows) {
        s_model_rows = heap_caps_calloc(MODEL_ROWS_MAX, sizeof(*s_model_rows), MALLOC_CAP_SPIRAM);
    }
    cJSON *results = model_test_results();
    static const model_test_kind_t kinds[3] = { MODEL_TEST_KIND_LLM, MODEL_TEST_KIND_STT, MODEL_TEST_KIND_TTS };
    if (kinds[list] == MODEL_TEST_KIND_LLM) {
        model_list_llm(s_model_lists[list], results);
    } else {
        model_list_speech(s_model_lists[list], kinds[list], results);
    }
    cJSON_Delete(results);
    lv_obj_scroll_to_y(s_model_lists[list], 0, LV_ANIM_OFF);
}

/* The scrollable list of a page of models, between the line under the title and the arrows */
static void model_list_create(int index)
{
    lv_obj_t *list = lv_obj_create(ui_PanelSettings);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, lv_pct(100), 124);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(list, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(list, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(list, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    s_model_lists[index] = list;
    settings_row_add(list, SETTINGS_PAGE_MODELS + index);
}

/* ---------- Debug page: the traces on or off (Debug), what they keep, and the Dump button ---------- */

static lv_obj_t *s_debug_switch = NULL;
static lv_obj_t *s_debug_label = NULL;
static lv_obj_t *s_debug_info = NULL;
static lv_obj_t *s_dump_label = NULL;

static void debug_page_refresh(void)
{
    if (!s_debug_info) {
        return;
    }
    const app_lang_t *lang = app_lang_get();
    if (debug_trace_on()) {
        lv_obj_add_state(s_debug_switch, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(s_debug_switch, LV_STATE_CHECKED);
    }
    size_t lines, bytes;
    debug_trace_stats(&lines, &bytes);
    char text[64];
    snprintf(text, sizeof(text), lang->settings_debug_info, (unsigned)lines, (unsigned)((bytes + 1023) / 1024));
    lv_label_set_text(s_debug_info, text);
    lv_label_set_text(s_debug_label, lang->settings_debug);
    lv_label_set_text(s_dump_label, lang->settings_dump);
}

static void debug_switch_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state(s_debug_switch, LV_STATE_CHECKED);
    settings_get_parameter()->debug = on;
    settings_save_str("Debug", on ? "on" : "off");
    debug_trace_set(on);
    debug_page_refresh();
}

static void dump_clicked_cb(lv_event_t *e)
{
    lv_label_set_text(s_dump_label, "...");
    assistant_request_dump();
}

/* A row of the Debug page (no dropdown): the switch, the lines kept, or the button */
static lv_obj_t *debug_row_create(lv_flex_align_t align)
{
    lv_obj_t *row = lv_obj_create(ui_PanelSettings);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), lv_pct(19));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, align, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(row, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(row, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    settings_row_add(row, SETTINGS_PAGE_DEBUG);
    return row;
}

static void debug_page_create(void)
{
    lv_obj_t *row = debug_row_create(LV_FLEX_ALIGN_SPACE_BETWEEN);
    s_debug_label = lv_label_create(row);
    lv_obj_set_style_text_color(s_debug_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_debug_label, &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    s_debug_switch = lv_switch_create(row);
    lv_obj_set_style_bg_color(s_debug_switch, lv_color_hex(0x6E47BD), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_debug_switch, debug_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    row = debug_row_create(LV_FLEX_ALIGN_SPACE_BETWEEN);
    s_debug_info = lv_label_create(row);
    lv_obj_set_style_text_color(s_debug_info, lv_color_hex(MODEL_ROW_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_debug_info, &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_t *btn = lv_btn_create(row);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x6E47BD), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn, dump_clicked_cb, LV_EVENT_CLICKED, NULL);
    s_dump_label = lv_label_create(btn);
    lv_obj_set_style_text_font(s_dump_label, &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(s_dump_label);
    debug_page_refresh();
}

static void settings_screen_setup(void)
{
    const app_lang_t *lang = app_lang_get();
    const sys_param_t *param = settings_get_parameter();
    char *options = dropdown_options();

    lv_obj_set_style_pad_row(ui_PanelSettings, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    settings_row_add(ui_PanelSettingsRegion, 0);

    // The generated "Region" row (unused) becomes the language row
    label_localize(ui_LabelSettingsRegion, lang->settings_language, &ui_font_Latin16);
    lv_obj_set_flex_align(ui_PanelSettingsRegion, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ui_PanelSettingsRegion, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ui_PanelSettingsRegion, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_width(ui_DropdownSettingsRegion, lv_pct(40));
    lv_dropdown_set_symbol(ui_DropdownSettingsRegion, NULL);
    uint16_t lang_selected = settings_language_index(param->lang);
    s_lang_stored = lang_selected;
    // The languages offered (Languages of CONFIG.INI), by their names
    char *languages = dropdown_options();
    for (size_t i = 0; i < app_lang_count(); i++) {
        strlcat(languages, i ? "\n" : "", SETTINGS_OPTIONS_SIZE);
        strlcat(languages, app_lang_name(i), SETTINGS_OPTIONS_SIZE);
    }
    settings_dropdown_setup(ui_DropdownSettingsRegion, languages, lang_selected, settings_changed_cb, NULL);

    // Model row: the listed models, plus the current one if it was set in CONFIG.INI and is not listed
    s_model_dropdown = settings_row_create(lang->settings_model, lv_pct(62));
    s_model_row_label = lv_obj_get_child(lv_obj_get_parent(s_model_dropdown), 0);
    settings_row_add(lv_obj_get_parent(s_model_dropdown), 0);
    uint16_t model_selected = model_options(options, SETTINGS_OPTIONS_SIZE);
    s_model_stored = model_selected;
    settings_dropdown_setup(s_model_dropdown, options, model_selected, settings_changed_cb, NULL);

    // Second page: transcription (STT) and speech (TTS) models, then the voice, all applied immediately (no Confirm
    // button); the short labels leave the dropdowns room for the models' names
    const char *speech_texts[2] = { lang->settings_stt, lang->settings_tts };
    for (size_t i = 0; i < 2; i++) {
        settings_speech_t *row = &s_settings_speech[i];
        row->dropdown = settings_row_create(speech_texts[i], lv_pct(78));
        row->label = lv_obj_get_child(lv_obj_get_parent(row->dropdown), 0);
        settings_row_add(lv_obj_get_parent(row->dropdown), 1);
        settings_dropdown_setup(row->dropdown, "-", 0, settings_speech_changed_cb, row);
        speech_options_refresh(row);
    }

    lv_obj_t *voice_dropdown = settings_row_create(lang->settings_voice, lv_pct(58));
    s_voice_dropdown = voice_dropdown;
    s_voice_row_label = lv_obj_get_child(lv_obj_get_parent(voice_dropdown), 0);
    settings_row_add(lv_obj_get_parent(voice_dropdown), 1);
    settings_dropdown_setup(voice_dropdown, "-", 0, settings_voice_changed_cb, NULL);
    voice_options_refresh();

    // Pages 3 to 5: the LLM, STT and TTS models
    for (int i = 0; i < 3; i++) {
        model_list_create(i);
    }
    // Page 6: the debug traces
    debug_page_create();

    // Arrows to the previous / next page at the bottom left and right
    settings_arrow_create(LV_ALIGN_BOTTOM_LEFT, 10, LV_SYMBOL_LEFT, -1);
    settings_arrow_create(LV_ALIGN_BOTTOM_RIGHT, -10, LV_SYMBOL_RIGHT, 1);
    lv_obj_add_event_cb(ui_ScreenSettings, settings_screen_loaded_cb, LV_EVENT_SCREEN_LOAD_START, NULL);
    lv_obj_add_event_cb(ui_ScreenSettings, settings_screen_unload_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);

    // Confirm button (bottom middle, first page only), shown once the language or the model differs from the
    // stored settings; not confirmed, they are dropped when the page or the screen is left
    s_save_btn = settings_button_create(LV_ALIGN_BOTTOM_MID, 0);
    lv_obj_add_event_cb(s_save_btn, settings_save_clicked_cb, LV_EVENT_CLICKED, NULL);
    s_save_label = lv_label_create(s_save_btn);
    lv_label_set_text(s_save_label, lang->settings_confirm);
    lv_obj_set_style_text_font(s_save_label, &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(s_save_label);
    settings_page_show(0);
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
        lv_label_set_text(s_settings_speech[0].label, lang->settings_stt);
        lv_label_set_text(s_settings_speech[1].label, lang->settings_tts);
        lv_label_set_text(s_save_label, lang->settings_confirm);
        model_list_show(s_settings_page);   // the title of the page
        debug_page_refresh();
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
    uint16_t lang_selected = settings_language_index(param->lang);
    s_lang_stored = lang_selected;
    lv_dropdown_set_selected(ui_DropdownSettingsRegion, lang_selected);
    for (size_t i = 0; i < llm_model_count; i++) {
        if (strcmp(param->chat_model, llm_models[i].id) == 0) {
            s_model_stored = i;
            lv_dropdown_set_selected(s_model_dropdown, i);
        }
    }
    // The provider's icon on the robot's forehead (model changed, or set at start-up after the screens were made)
    head_icons_refresh();
    // Speech models changed by voice ("change de synthèse vocale"), then their voices ("change de voix")
    speech_options_refresh(&s_settings_speech[0]);
    speech_options_refresh(&s_settings_speech[1]);
    voice_options_refresh();
    if (s_settings_page >= SETTINGS_PAGE_MODELS && s_model_test_row < 0) {
        model_list_show(s_settings_page);   // the model in use changed by voice
    }
    confirm_button_update(false);
    bsp_display_unlock();
}

void ui_ctrl_show_message(const char *text)
{
    bsp_display_lock(0);
    lv_obj_t *box = lv_msgbox_create(NULL, NULL, text, NULL, false);
    lv_obj_set_width(box, 290);
    lv_obj_set_style_text_font(lv_msgbox_get_text(box), &ui_font_Latin16, LV_PART_MAIN);
    lv_obj_center(box);
    bsp_display_unlock();
}

/* ---------- Problems of a language file: title, text (scrolled with the finger), "Skip" and "Config" ---------- */

#define PROBLEMS_BTN_W      96
#define PROBLEMS_BTN_H      28
#define PROBLEMS_MARGIN     16

static lv_obj_t *s_problems_screen = NULL;
static lv_obj_t *s_problems_title = NULL;
static lv_obj_t *s_problems_box = NULL;     /* scrollable */
static lv_obj_t *s_problems_text = NULL;
static lv_obj_t *s_problems_back = NULL;    /* the screen shown before */

static void problems_skip_cb(lv_event_t *e)
{
    lv_scr_load(s_problems_back ? s_problems_back : ui_ScreenListen);
}

static void problems_config_cb(lv_event_t *e)
{
    assistant_request_config();     // the main task saves, then restarts in configuration mode
}

static void problems_button(const char *text, lv_align_t align, lv_coord_t x, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(s_problems_screen);
    lv_obj_set_size(btn, PROBLEMS_BTN_W, PROBLEMS_BTN_H);
    lv_obj_align(btn, align, x, -12);
    lv_obj_set_style_radius(btn, 17, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_color(label, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(label, &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(label, text);
    lv_obj_center(label);
}

static void problems_screen_create(void)
{
    s_problems_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(s_problems_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_img_src(s_problems_screen, &ui_img_setup_bg_png, LV_PART_MAIN | LV_STATE_DEFAULT);

    s_problems_title = lv_label_create(s_problems_screen);
    lv_obj_set_style_text_font(s_problems_title, &ui_font_Latin20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_problems_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_problems_title, LV_ALIGN_TOP_MID, 0, 14);

    // The text between the title and the buttons, scrolled with the finger when too long
    s_problems_box = lv_obj_create(s_problems_screen);
    lv_obj_remove_style_all(s_problems_box);
    lv_obj_set_size(s_problems_box, 320 - 2 * PROBLEMS_MARGIN, 240 - 48 - PROBLEMS_BTN_H - 12 - 8);
    lv_obj_align(s_problems_box, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_scroll_dir(s_problems_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_problems_box, LV_SCROLLBAR_MODE_AUTO);

    s_problems_text = lv_label_create(s_problems_box);
    lv_obj_set_width(s_problems_text, lv_pct(100));
    lv_label_set_long_mode(s_problems_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_problems_text, &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_problems_text, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);

    problems_button("Skip", LV_ALIGN_BOTTOM_LEFT, PROBLEMS_MARGIN, problems_skip_cb);
    problems_button("Config", LV_ALIGN_BOTTOM_RIGHT, -PROBLEMS_MARGIN, problems_config_cb);
}

void ui_ctrl_show_problems(const char *title, const char *text)
{
    bsp_display_lock(0);
    if (!s_problems_screen) {
        problems_screen_create();
    }
    lv_label_set_text(s_problems_title, title);
    lv_label_set_text(s_problems_text, text);
    lv_obj_scroll_to_y(s_problems_box, 0, LV_ANIM_OFF);
    if (lv_scr_act() != s_problems_screen) {
        s_problems_back = lv_scr_act();
    }
    lv_scr_load(s_problems_screen);
    bsp_display_unlock();
}

void ui_ctrl_show_clock(bool shown)
{
    bsp_display_lock(0);
    app_clock_show(shown);
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
    // One line exactly: a label lower than the font's line scrolls a short question vertically
    lv_obj_set_height(ui_LabelReplyQuestion, lv_font_get_line_height(&ui_font_Latin20));
    lv_obj_set_style_text_font(ui_LabelReplyContent, &ui_font_Latin20, LV_PART_MAIN | LV_STATE_DEFAULT);
    settings_screen_setup();
    ui_ctrl_apply_texts();
}

/* ---------- Costs: the conversation's at the top right; a tap opens their page ---------- */

#define COSTS_REFRESH_MS    1000
#define COSTS_COLOR         0xB9CCF5

static lv_obj_t *s_cost_label = NULL;
static lv_obj_t *s_cost_day_label = NULL;     /* below it: the cost of the day, "[0,123 €]" */
static lv_obj_t *s_cost_screen = NULL;
static lv_obj_t *s_cost_table = NULL;
static lv_obj_t *s_cost_title = NULL;
/* The periods shown one at a time: en cours, ce jour, ce mois */
static lv_obj_t *s_cost_tabs[3];
static cost_scope_t s_cost_scope = COST_SCOPE_CONVERSATION;
#define COSTS_TAB_DIM       0x8A97BC

/* Refreshed every second (LVGL task): the cost since the last wake-up, and below it the cost of the day */
static void cost_label_refresh(lv_timer_t *timer)
{
    static double shown = -1, shown_day = -1;
    char text[32], number[24];
    double total = costs_conversation(NULL);
    if (total != shown) {
        shown = total;
        costs_format(text, sizeof(text), total, true);
        lv_label_set_text(s_cost_label, text);
    }
    double day = costs_period(COST_PERIOD_TODAY, NULL, NULL);
    if (day != shown_day) {
        shown_day = day;
        costs_format_short(number, sizeof(number), day, true);
        snprintf(text, sizeof(text), "[%s]", number);
        lv_label_set_text(s_cost_day_label, text);
    }
}

/* A row of the table: STT, LLM (with the tools billed per call), TTS, total */
static void cost_row(int row, const char *name, const double values[4], bool amounts)
{
    char text[24];
    lv_table_set_cell_value(s_cost_table, row, 0, name);
    for (int c = 0; c < 4; c++) {
        if (amounts) {
            costs_format(text, sizeof(text), values[c], false);
        } else {
            costs_format_tokens(text, sizeof(text), values[c]);
        }
        lv_table_set_cell_value(s_cost_table, row, c + 1, text);
    }
}

static void cost_page_fill(void)
{
    const app_lang_t *lang = app_lang_get();
    char title[48], symbol[32];
    costs_format(symbol, sizeof(symbol), 0, true);     // "0,0000 €": the currency after the number
    const char *currency = strrchr(symbol, ' ') ? strrchr(symbol, ' ') + 1 : (symbol[0] == '$' ? "$" : "");
    snprintf(title, sizeof(title), "%s (%s)", lang->costs_title, currency);
    lv_label_set_text(s_cost_title, title);

    // The tabs: the period shown in white, underlined; the others dimmed
    const char *names[3] = { lang->costs_conversation, lang->costs_today, lang->costs_month };
    for (int i = 0; i < 3; i++) {
        bool shown = (i == (int)s_cost_scope);
        lv_label_set_text(s_cost_tabs[i], names[i]);
        lv_obj_set_style_text_color(s_cost_tabs[i], lv_color_hex(shown ? 0xFFFFFF : COSTS_TAB_DIM),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_decor(s_cost_tabs[i], shown ? LV_TEXT_DECOR_UNDERLINE : LV_TEXT_DECOR_NONE,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    lv_table_set_cell_value(s_cost_table, 0, 0, "");
    lv_table_set_cell_value(s_cost_table, 0, 1, "STT");
    lv_table_set_cell_value(s_cost_table, 0, 2, "LLM");
    lv_table_set_cell_value(s_cost_table, 0, 3, "TTS");
    lv_table_set_cell_value(s_cost_table, 0, 4, lang->costs_total);
    cost_detail_t d[COST_KINDS];
    costs_detail(s_cost_scope, d);
    // LLM with the tools billed per call (their tokens are in the answer's)
    const double cost[4] = { d[COST_STT].cost, d[COST_LLM].cost + d[COST_TOOL].cost, d[COST_TTS].cost,
                             d[COST_STT].cost + d[COST_LLM].cost + d[COST_TOOL].cost + d[COST_TTS].cost };
    const double in[4] = { d[COST_STT].in, d[COST_LLM].in, d[COST_TTS].in, d[COST_STT].in + d[COST_LLM].in + d[COST_TTS].in };
    const double out[4] = { d[COST_STT].out, d[COST_LLM].out, d[COST_TTS].out,
                            d[COST_STT].out + d[COST_LLM].out + d[COST_TTS].out };
    cost_row(1, lang->costs_cost, cost, true);
    cost_row(2, "In", in, false);
    cost_row(3, "Out", out, false);
}

static void cost_tab_cb(lv_event_t *e)
{
    s_cost_scope = (cost_scope_t)(intptr_t)lv_event_get_user_data(e);
    cost_page_fill();
}

static void cost_page_back_cb(lv_event_t *e)
{
    lv_scr_load(ui_ScreenListen);
}

static void cost_page_create(void)
{
    s_cost_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(s_cost_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_img_src(s_cost_screen, &ui_img_setup_bg_png, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *back = lv_img_create(s_cost_screen);
    lv_img_set_src(back, &ui_img_settings_back_png);
    lv_obj_set_pos(back, 10, 10);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_event_cb(back, cost_page_back_cb, LV_EVENT_CLICKED, NULL);

    s_cost_title = lv_label_create(s_cost_screen);
    lv_obj_set_style_text_font(s_cost_title, &ui_font_Latin20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_cost_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_cost_title, LV_ALIGN_TOP_MID, 0, 14);

    // The periods, side by side: a tap shows one
    lv_obj_t *tabs = lv_obj_create(s_cost_screen);
    lv_obj_remove_style_all(tabs);
    lv_obj_set_size(tabs, 312, 30);
    lv_obj_align(tabs, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(tabs, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 3; i++) {
        s_cost_tabs[i] = lv_label_create(tabs);
        lv_obj_set_style_text_font(s_cost_tabs[i], &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_flag(s_cost_tabs[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(s_cost_tabs[i], 10);
        lv_obj_add_event_cb(s_cost_tabs[i], cost_tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    s_cost_table = lv_table_create(s_cost_screen);
    lv_table_set_col_cnt(s_cost_table, 5);
    lv_table_set_row_cnt(s_cost_table, 4);
    lv_table_set_col_width(s_cost_table, 0, 52);
    for (int c = 1; c < 5; c++) {
        lv_table_set_col_width(s_cost_table, c, 65);
    }
    lv_obj_set_style_text_font(s_cost_table, &ui_font_Latin14, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_cost_table, lv_color_hex(0xFFFFFF), LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_cost_table, LV_TEXT_ALIGN_RIGHT, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_cost_table, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_cost_table, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_cost_table, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_cost_table, 1, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_cost_table, lv_color_hex(0x5A6A9A), LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_border_side(s_cost_table, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_cost_table, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(s_cost_table, 6, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(s_cost_table, 6, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(s_cost_table, 2, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(s_cost_table, 3, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_size(s_cost_table, 312, LV_SIZE_CONTENT);
    lv_obj_align(s_cost_table, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_clear_flag(s_cost_table, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

static void cost_label_clicked_cb(lv_event_t *e)
{
    if (!s_cost_screen) {
        cost_page_create();
    }
    s_cost_scope = COST_SCOPE_CONVERSATION;    // opened from the conversation's cost: shown first
    cost_page_fill();
    lv_scr_load(s_cost_screen);
}

/* ---------- Model in use: provider icon and model name at the bottom left, while listening and thinking ---------- */

static const void *logo_image(ui_ctrl_logo_t logo)
{
    return logo == UI_CTRL_LOGO_WEB ? (const void *)&ui_img_reply_web :
           logo == UI_CTRL_LOGO_LOCAL ? (const void *)&ui_img_reply_local :
           logo == UI_CTRL_LOGO_CLAUDE ? (const void *)&ui_img_reply_claude :
           logo == UI_CTRL_LOGO_GEMINI ? (const void *)&ui_img_reply_gemini :
           logo == UI_CTRL_LOGO_OPENROUTER ? (const void *)&ui_img_reply_openrouter :
           logo == UI_CTRL_LOGO_XAI ? (const void *)&ui_img_reply_xai :
           logo == UI_CTRL_LOGO_MISTRAL ? (const void *)&ui_img_reply_mistral :
           logo == UI_CTRL_LOGO_PERPLEXITY ? (const void *)&ui_img_reply_perplexity :
           (const void *)&ui_img_reply_chatgpt_logo_png;
}

static ui_ctrl_logo_t provider_logo(llm_provider_t provider)
{
    return provider == LLM_PROVIDER_OPENAI ? UI_CTRL_LOGO_OPENAI :
           provider == LLM_PROVIDER_GEMINI ? UI_CTRL_LOGO_GEMINI :
           provider == LLM_PROVIDER_OPENROUTER ? UI_CTRL_LOGO_OPENROUTER :
           provider == LLM_PROVIDER_XAI ? UI_CTRL_LOGO_XAI :
           provider == LLM_PROVIDER_MISTRAL ? UI_CTRL_LOGO_MISTRAL :
           provider == LLM_PROVIDER_PERPLEXITY ? UI_CTRL_LOGO_PERPLEXITY : UI_CTRL_LOGO_CLAUDE;
}

static lv_obj_t *s_model_icon = NULL;
static lv_obj_t *s_model_name = NULL;
/* The same icon on the forehead of the robot asleep, listening and thinking: children of the screens of the eyes,
 * drawn above them (outside their area), so that they follow their moves with the head */
static lv_obj_t *s_head_icons[3] = { NULL };
/* Next to them, a gear while the reasoning mode is on ("j'ai une question difficile") */
static lv_obj_t *s_head_badges[3] = { NULL };
#define HEAD_BADGE_COLOR    0xFFB300

/*
 * The area redrawn when a screen of the eyes moves: its own, plus this margin, which must reach the top of the icon
 * drawn above it (else the icon leaves its old pixels behind)
 */
static void eye_screen_ext_draw_cb(lv_event_t *e)
{
    lv_coord_t *size = lv_event_get_param(e);
    lv_coord_t needed = HEAD_LOGO_GAP_Y + 21;   // the icons are 20 x 21
    if (*size < needed) {
        *size = needed;
    }
}

static void head_icons_create(void)
{
    lv_obj_t *eye_screens[3] = { ui_Image3, ui_ImageListenEyeScreen, ui_ImageGetEyeScreen };
    for (size_t i = 0; i < 3; i++) {
        lv_obj_add_flag(eye_screens[i], LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_add_event_cb(eye_screens[i], eye_screen_ext_draw_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
        lv_obj_refresh_ext_draw_size(eye_screens[i]);
        s_head_icons[i] = lv_img_create(eye_screens[i]);
        lv_img_set_src(s_head_icons[i], logo_image(provider_logo(llm_provider())));
        lv_obj_align(s_head_icons[i], LV_ALIGN_OUT_TOP_MID, HEAD_LOGO_X, -HEAD_LOGO_GAP_Y);
        lv_obj_clear_flag(s_head_icons[i], LV_OBJ_FLAG_CLICKABLE);
        s_head_badges[i] = lv_label_create(eye_screens[i]);
        lv_label_set_text(s_head_badges[i], LV_SYMBOL_SETTINGS);
        lv_obj_set_style_text_font(s_head_badges[i], &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_head_badges[i], lv_color_hex(HEAD_BADGE_COLOR), LV_PART_MAIN);
        lv_obj_align_to(s_head_badges[i], s_head_icons[i], LV_ALIGN_OUT_RIGHT_MID, 3, 0);
        lv_obj_clear_flag(s_head_badges[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_head_badges[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* The icon of the provider in use, on the foreheads */
static void head_icons_refresh(void)
{
    const void *src = logo_image(provider_logo(llm_provider()));
    for (size_t i = 0; i < 3; i++) {
        if (s_head_icons[i] && lv_img_get_src(s_head_icons[i]) != src) {
            lv_img_set_src(s_head_icons[i], src);
        }
        if (s_head_badges[i] && lv_obj_has_flag(s_head_badges[i], LV_OBJ_FLAG_HIDDEN) == llm_reasoning()) {
            if (llm_reasoning()) {
                lv_obj_clear_flag(s_head_badges[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_head_badges[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void model_row_create(void)
{
    s_model_icon = lv_img_create(ui_ScreenListen);
    lv_img_set_src(s_model_icon, logo_image(UI_CTRL_LOGO_CLAUDE));
    lv_obj_align(s_model_icon, LV_ALIGN_BOTTOM_LEFT, MODEL_ROW_LEFT_X, MODEL_ROW_BOTTOM_Y);
    lv_obj_clear_flag(s_model_icon, LV_OBJ_FLAG_CLICKABLE);

    s_model_name = lv_label_create(ui_ScreenListen);
    lv_obj_set_style_text_font(s_model_name, &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_model_name, lv_color_hex(MODEL_ROW_COLOR), LV_PART_MAIN | LV_STATE_DEFAULT);
    // A long name (free OpenRouter models) ends with "..." rather than leaving the screen
    lv_label_set_long_mode(s_model_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_model_name, 320 - MODEL_ROW_LEFT_X - 20 - MODEL_ROW_GAP_X - MODEL_ROW_LEFT_X);
    lv_obj_align_to(s_model_name, s_model_icon, LV_ALIGN_OUT_RIGHT_MID, MODEL_ROW_GAP_X, 0);
    lv_label_set_text(s_model_name, "");

    lv_obj_add_flag(s_model_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_model_name, LV_OBJ_FLAG_HIDDEN);
}

/* Shown with the model in use (its name in AGENTS.INI, else its id), or hidden */
static void model_row_show(bool shown)
{
    if (!s_model_icon) {
        return;
    }
    if (!shown) {
        lv_obj_add_flag(s_model_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_model_name, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const char *id = settings_get_parameter()->chat_model;
    const char *name = id;
    for (size_t i = 0; i < llm_model_count; i++) {
        if (strcmp(llm_models[i].id, id) == 0) {
            name = llm_models[i].label;
            break;
        }
    }
    lv_img_set_src(s_model_icon, logo_image(provider_logo(llm_provider())));
    lv_label_set_text(s_model_name, name);
    lv_obj_clear_flag(s_model_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_model_name, LV_OBJ_FLAG_HIDDEN);
}

/*
 * Listening while the reply stays shown (it asks something): bars rising and falling beside the robot head at its
 * top right
 */
#define LISTEN_BARS_COLOR       0x4FC3F7
#define LISTEN_BARS             3
static lv_obj_t *s_listen_head = NULL;
static lv_obj_t *s_listen_bars[LISTEN_BARS];

static lv_obj_t *listen_head_part(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, lv_coord_t radius, uint32_t color)
{
    lv_obj_t *part = lv_obj_create(parent);
    lv_obj_remove_style_all(part);
    lv_obj_set_size(part, w, h);
    lv_obj_set_style_radius(part, radius, LV_PART_MAIN);
    lv_obj_set_style_bg_color(part, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(part, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(part, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return part;
}

static void listen_head_create(void)
{
    s_listen_head = listen_head_part(ui_PanelReply, 50, 32, 0, 0);
    lv_obj_set_style_bg_opa(s_listen_head, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_align(s_listen_head, LV_ALIGN_TOP_RIGHT, -8, 6);
    // The bars beside the robot head of the reply's picture: animated while shown
    for (int i = 0; i < LISTEN_BARS; i++) {
        s_listen_bars[i] = listen_head_part(s_listen_head, 3, 4, 1, LISTEN_BARS_COLOR);
        lv_obj_align(s_listen_bars[i], LV_ALIGN_LEFT_MID, 34 + i * 6, 5);
    }
    lv_obj_add_flag(s_listen_head, LV_OBJ_FLAG_HIDDEN);
}

static void listen_bar_anim_cb(void *bar, int32_t height)
{
    lv_obj_set_height(bar, height);
}

/* Shown or hidden (display locked) */
static void listen_head_show(bool on)
{
    if (!s_listen_head || lv_obj_has_flag(s_listen_head, LV_OBJ_FLAG_HIDDEN) == !on) {
        return;
    }
    for (int i = 0; i < LISTEN_BARS; i++) {
        lv_anim_del(s_listen_bars[i], listen_bar_anim_cb);
        lv_obj_set_height(s_listen_bars[i], 4);
        if (on) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, s_listen_bars[i]);
            lv_anim_set_exec_cb(&a, listen_bar_anim_cb);
            lv_anim_set_values(&a, 4, 16);
            lv_anim_set_time(&a, 350);
            lv_anim_set_playback_time(&a, 350);
            lv_anim_set_delay(&a, i * 120);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
        }
    }
    if (on) {
        lv_obj_move_foreground(s_listen_head);
        lv_obj_clear_flag(s_listen_head, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_listen_head, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_ctrl_reply_listening(bool on)
{
    bsp_display_lock(0);
    listen_head_show(on && current_panel == UI_CTRL_PANEL_REPLY);
    bsp_display_unlock();
}

/* Bottom left of the reply: a blinking arrow down while a list shown goes on below the screen */
#define SCROLL_ARROW_BG_COLOR   0x1F2A44
#define SCROLL_ARROW_COLOR      0xFFFFFF
static lv_obj_t *s_scroll_arrow = NULL;

static void scroll_arrow_opa_cb(void *obj, int32_t opa)
{
    lv_obj_set_style_opa(obj, opa, LV_PART_MAIN);
}

static void reply_scrolled_cb(lv_event_t *e)
{
    s_manual_idle_us = esp_timer_get_time();
    scroll_arrow_update();
}

/*
 * A finger on the reply (pressed, or the text scrolling under it): the delays before sleep are paused (the listening
 * without speech, and the list's idle time)
 */
static bool s_reply_pressed = false;
static bool s_reply_scrolling = false;

static void reply_touch_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_reply_pressed = true;
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        s_reply_pressed = false;
        break;
    case LV_EVENT_SCROLL_BEGIN:
        s_reply_scrolling = true;
        break;
    case LV_EVENT_SCROLL_END:
        s_reply_scrolling = false;
        break;
    default:
        return;
    }
    s_manual_idle_us = esp_timer_get_time();
    app_sr_touch_hold(s_reply_pressed || s_reply_scrolling);
}

static void scroll_arrow_create(void)
{
    s_scroll_arrow = lv_obj_create(ui_PanelReply);
    lv_obj_remove_style_all(s_scroll_arrow);
    lv_obj_set_size(s_scroll_arrow, 26, 26);
    lv_obj_set_style_radius(s_scroll_arrow, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_scroll_arrow, lv_color_hex(SCROLL_ARROW_BG_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scroll_arrow, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(s_scroll_arrow, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_obj_clear_flag(s_scroll_arrow, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *symbol = lv_label_create(s_scroll_arrow);
    lv_label_set_text(symbol, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(symbol, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(symbol, lv_color_hex(SCROLL_ARROW_COLOR), LV_PART_MAIN);
    lv_obj_center(symbol);
    lv_obj_add_flag(s_scroll_arrow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(ui_ContainerReplyContent, reply_scrolled_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(ui_ContainerReplyContent, reply_touch_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_PanelReply, reply_touch_cb, LV_EVENT_ALL, NULL);
}

/* Shown, blinking, while a list not read goes on below the screen (display locked) */
static void scroll_arrow_update(void)
{
    if (!s_scroll_arrow) {
        return;
    }
    bool on = s_manual_scroll && current_panel == UI_CTRL_PANEL_REPLY &&
              lv_obj_get_scroll_bottom(ui_ContainerReplyContent) > 0;
    if (lv_obj_has_flag(s_scroll_arrow, LV_OBJ_FLAG_HIDDEN) == !on) {
        return;
    }
    lv_anim_del(s_scroll_arrow, scroll_arrow_opa_cb);
    if (on) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_scroll_arrow);
        lv_anim_set_exec_cb(&a, scroll_arrow_opa_cb);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
        lv_anim_set_time(&a, 500);
        lv_anim_set_playback_time(&a, 500);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        lv_obj_move_foreground(s_scroll_arrow);
        lv_obj_clear_flag(s_scroll_arrow, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_scroll_arrow, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_ctrl_reply_list_shown(void)
{
    bsp_display_lock(0);
    s_manual_scroll = true;
    s_manual_idle_us = esp_timer_get_time();
    reply_scroll_stop();
    lv_obj_update_layout(ui_ContainerReplyContent);
    lv_obj_scroll_to_y(ui_ContainerReplyContent, 0, LV_ANIM_OFF);
    scroll_arrow_update();
    bsp_display_unlock();
}

static void cost_label_create(void)
{
    s_cost_label = lv_label_create(ui_ScreenListen);
    lv_obj_set_style_text_font(s_cost_label, &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_cost_label, lv_color_hex(COSTS_COLOR), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_cost_label, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_ext_click_area(s_cost_label, 12);
    lv_obj_add_flag(s_cost_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_cost_label, cost_label_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(s_cost_label, "");

    s_cost_day_label = lv_label_create(ui_ScreenListen);
    lv_obj_set_style_text_font(s_cost_day_label, &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_cost_day_label, lv_color_hex(COSTS_COLOR), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_cost_day_label, LV_ALIGN_TOP_RIGHT, -10, 32);
    lv_obj_set_ext_click_area(s_cost_day_label, 8);
    lv_obj_add_flag(s_cost_day_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_cost_day_label, cost_label_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(s_cost_day_label, "");
    cost_label_refresh(NULL);
    lv_timer_create(cost_label_refresh, COSTS_REFRESH_MS, NULL);
}

void ui_ctrl_init(void)
{
    bsp_display_lock(0);

    ui_init();
    ui_ctrl_localize();
    setup_button_setup();
    app_clock_create(ui_PanelSleep, app_lang_get()->weekdays);
    app_clock_show(settings_get_parameter()->show_clock);
    cost_label_create();
    listen_head_create();
    scroll_arrow_create();
    model_row_create();
    head_icons_create();
    ui_radio_init();
    // Taps on the answer land on the panel or on the scrolling text container (both clickable)
    lv_obj_add_event_cb(ui_PanelReply, reply_double_tap_cb, LV_EVENT_CLICKED, NULL);
    // While listening or thinking: a double tap drops it, back to sleep
    lv_obj_add_event_cb(ui_PanelListen, cancel_double_tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_PanelGet, cancel_double_tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_ContainerReplyContent, reply_double_tap_cb, LV_EVENT_CLICKED, NULL);
    // A long press anywhere (asleep, listening, thinking, answering): a mark in the debug traces, if on
    lv_obj_t *const marked[] = { ui_PanelSleep, ui_PanelListen, ui_PanelGet, ui_PanelReply, ui_ContainerReplyContent };
    for (size_t i = 0; i < sizeof(marked) / sizeof(marked[0]); i++) {
        lv_obj_add_event_cb(marked[i], mark_long_pressed_cb, LV_EVENT_LONG_PRESSED, NULL);
    }

    scroll_timer_handle = lv_timer_create(reply_content_scroll_timer_handler, REPLY_SCROLL_TIMER_INTERVAL_MS, NULL);
    lv_timer_pause(scroll_timer_handle);

    lv_timer_create(wifi_check_timer_handler, WIFI_CHECK_TIMER_INTERVAL_S * 1000, NULL);

    bsp_display_unlock();
}

/* ---------- Welcome and configuration screens: their button counts down, then is pressed ---------- */

#define SETUP_COUNTDOWN_S   60
#define RESET_COUNTDOWN_S   30
#define COUNTDOWN_FMT       "OK (%d)"
#define COUNTDOWN_BTN_WIDTH 96      /* the generated 72 px is sized for "OK" */

/* One countdown at a time: the button of the screen shown */
static lv_timer_t *s_setup_countdown = NULL;
static int s_setup_seconds_left = 0;
static lv_obj_t *s_countdown_btn = NULL;
static lv_obj_t *s_countdown_label = NULL;

static void setup_countdown_stop(void)
{
    if (s_setup_countdown) {
        lv_timer_del(s_setup_countdown);
        s_setup_countdown = NULL;
    }
}

/* The button is pressed (tap, wake word or end of the countdown), or its screen left: the countdown stops */
static void setup_button_clicked_cb(lv_event_t *e)
{
    setup_countdown_stop();
}

static void setup_countdown_cb(lv_timer_t *timer)
{
    if (--s_setup_seconds_left > 0) {
        lv_label_set_text_fmt(s_countdown_label, COUNTDOWN_FMT, s_setup_seconds_left);
        return;
    }
    // As if the button had been tapped
    setup_countdown_stop();
    lv_event_send(s_countdown_btn, LV_EVENT_CLICKED, NULL);
}

static void countdown_start(lv_obj_t *btn, lv_obj_t *label, int seconds)
{
    setup_countdown_stop();
    s_countdown_btn = btn;
    s_countdown_label = label;
    s_setup_seconds_left = seconds;
    lv_label_set_text_fmt(label, COUNTDOWN_FMT, seconds);
    s_setup_countdown = lv_timer_create(setup_countdown_cb, 1000, NULL);
}

/* Shown with the steps, once the Wi-Fi is connected */
static void setup_countdown_start(void)
{
    countdown_start(ui_ButtonSetup, ui_LabelSetupBtn, SETUP_COUNTDOWN_S);
}

/* Configuration screens (from Settings, or from the Wi-Fi screen): the countdown starts when shown */
static void reset_screen_loaded_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == ui_ScreenReset) {
        countdown_start(ui_ButtonResetConfirm, ui_LabelSetupBtn2, RESET_COUNTDOWN_S);
    } else {
        countdown_start(ui_ButtonWifiResetConfirm, ui_LabelSetupBtn1, RESET_COUNTDOWN_S);
    }
}

/* A button at the bottom right, its "OK" followed by the countdown: "OK (30)" */
static void countdown_button_setup(lv_obj_t *btn, lv_obj_t *label, int seconds)
{
    lv_obj_set_align(btn, LV_ALIGN_BOTTOM_RIGHT);
    lv_obj_set_pos(btn, -16, -16);
    lv_obj_set_width(btn, COUNTDOWN_BTN_WIDTH);
    lv_label_set_text_fmt(label, COUNTDOWN_FMT, seconds);
    lv_obj_add_event_cb(btn, setup_button_clicked_cb, LV_EVENT_CLICKED, NULL);
}

/* ---------- Configuration screen (from Settings): soft reset and factory reset icons, bottom left ---------- */

static lv_obj_t *s_restore_box = NULL;

/* Confirmation: the first button resets, the other one closes */
static void restore_box_cb(lv_event_t *e)
{
    lv_obj_t *box = lv_event_get_current_target(e);
    bool factory = (bool)(intptr_t)lv_event_get_user_data(e);
    bool confirmed = lv_msgbox_get_active_btn(box) == 0;
    lv_msgbox_close(box);
    s_restore_box = NULL;
    if (confirmed) {
        lv_label_set_text(ui_LabelSetupBtn2, app_lang_get()->restore_done);
        assistant_request_restore(factory);
    }
}

static void restore_clicked_cb(lv_event_t *e)
{
    if (s_restore_box) {
        return;
    }
    // The countdown would switch to the configuration mode under the confirmation
    setup_countdown_stop();
    lv_label_set_text(ui_LabelSetupBtn2, "OK");
    const app_lang_t *lang = app_lang_get();
    bool factory = (bool)(intptr_t)lv_event_get_user_data(e);
    static const char *btns[3];
    btns[0] = factory ? lang->factory_yes : lang->soft_yes;
    btns[1] = lang->restore_no;
    btns[2] = "";
    s_restore_box = lv_msgbox_create(NULL, factory ? lang->factory_title : lang->soft_title,
                                     factory ? lang->factory_text : lang->soft_text, btns, false);
    lv_obj_set_width(s_restore_box, 300);
    lv_obj_set_style_text_font(lv_msgbox_get_title(s_restore_box), &ui_font_Latin16, LV_PART_MAIN);
    lv_obj_set_style_text_font(lv_msgbox_get_text(s_restore_box), &ui_font_Latin14, LV_PART_MAIN);
    lv_obj_set_style_text_font(lv_msgbox_get_btns(s_restore_box), &ui_font_Latin14, LV_PART_ITEMS);
    // The buttons share the whole width: confirm at the bottom left, cancel at the bottom right
    lv_obj_set_size(lv_msgbox_get_btns(s_restore_box), lv_pct(100), 36);
    // A long text (a translation) scrolls with the finger rather than leaving the screen
    lv_obj_set_style_max_height(s_restore_box, 236, LV_PART_MAIN);
    lv_obj_center(s_restore_box);
    lv_obj_add_event_cb(s_restore_box, restore_box_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)factory);
}

/* An icon at the bottom left of the Configuration screen; x from the left edge */
static void restore_icon_create(const char *symbol, lv_color_t color, lv_coord_t x, bool factory)
{
    lv_obj_t *btn = lv_btn_create(ui_ScreenReset);
    lv_obj_set_size(btn, 40, 36);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, x, -16);
    lv_obj_set_style_radius(btn, 17, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn, restore_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)factory);
    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(icon);
}

static void setup_button_setup(void)
{
    // Soft reset (circular arrows), then factory reset (bin, red)
    restore_icon_create(LV_SYMBOL_REFRESH, lv_color_hex(0x6E47BD), 16, false);
    restore_icon_create(LV_SYMBOL_TRASH, lv_color_hex(0xC0392B), 66, true);
    countdown_button_setup(ui_ButtonSetup, ui_LabelSetupBtn, SETUP_COUNTDOWN_S);
    countdown_button_setup(ui_ButtonResetConfirm, ui_LabelSetupBtn2, RESET_COUNTDOWN_S);
    countdown_button_setup(ui_ButtonWifiResetConfirm, ui_LabelSetupBtn1, RESET_COUNTDOWN_S);
    lv_obj_add_event_cb(ui_ScreenReset, reset_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(ui_ScreenWifiReset, reset_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    // Back arrow: the countdown stops with its screen
    lv_obj_add_event_cb(ui_ScreenReset, setup_button_clicked_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);
    lv_obj_add_event_cb(ui_ScreenWifiReset, setup_button_clicked_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);
}

static void wifi_check_timer_handler(lv_timer_t *timer)
{
    if (WIFI_STATUS_CONNECTED_OK == wifi_connected_already()) {
        lv_obj_clear_flag(ui_PanelSetupSteps, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_PanelSetupWifi, LV_OBJ_FLAG_HIDDEN);
        lv_timer_del(timer);
        if (lv_scr_act() == ui_ScreenSetup) {
            setup_countdown_start();
        }
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
    // The assistant listens or answers: back from the page of the costs or from the radio screen
    if (s_cost_screen && lv_scr_act() == s_cost_screen) {
        lv_scr_load(ui_ScreenListen);
    }
    ui_radio_close();
    lv_obj_t *show_panel = NULL;
    lv_obj_t *hide_panel[3] = { NULL };

    switch (panel) {
    case UI_CTRL_PANEL_SLEEP:
        // Something to save: by the main task at once (not within the next second)
        if (llm_history_unsaved() || costs_unsaved()) {
            assistant_save_soon();
        }
        show_panel = ui_PanelSleep;
        hide_panel[0] = ui_PanelListen;
        hide_panel[1] = ui_PanelGet;
        hide_panel[2] = ui_PanelReply;
        lv_obj_clear_flag(ui_ImageListenSettings, LV_OBJ_FLAG_HIDDEN);
        listen_label_set_text(" ");
        // The assistant sleeps: a radio suspended by the conversation plays again
        radio_on_sleep();
        break;
    case UI_CTRL_PANEL_LISTEN:
        if (current_panel == UI_CTRL_PANEL_SLEEP) {
            costs_conversation_start();     // woken up: the cost of the conversation starts again
        }
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
        // "Entendu ..." until the transcript arrives, then "Je réfléchis ..." (main.c)
        listen_label_set_text(app_lang_get()->heard);
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
    // Listening over the reply: until the next panel
    listen_head_show(false);
    // A list scrolled with the finger: until the next reply
    if (panel != UI_CTRL_PANEL_REPLY) {
        s_manual_scroll = false;
        // The reply hidden under the finger: no release may come
        s_reply_pressed = false;
        s_reply_scrolling = false;
        app_sr_touch_hold(false);
    }
    scroll_arrow_update();
    // The model in use: shown while listening, "Entendu", "Je réfléchis"
    model_row_show(panel == UI_CTRL_PANEL_LISTEN || panel == UI_CTRL_PANEL_GET);
    head_icons_refresh();
    // The costs: shown while asleep, listening, "Entendu", "Je réfléchis"; hidden over the answer
    if (s_cost_label) {
        if (panel != UI_CTRL_PANEL_REPLY) {
            lv_obj_clear_flag(s_cost_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_cost_day_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_cost_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_cost_day_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    ESP_LOGI(TAG, "Swich to panel[%d]", panel);
}

bool ui_ctrl_is_asleep(void)
{
    return current_panel == UI_CTRL_PANEL_SLEEP;
}

bool ui_ctrl_screen_can_sleep(void)
{
    return current_panel == UI_CTRL_PANEL_SLEEP && lv_scr_act() == ui_ScreenListen;
}

void ui_ctrl_show_panel(ui_ctrl_panel_t panel, uint16_t timeout)
{
    bsp_display_lock(0);
    head_icons_refresh();       // the reasoning mode may have changed

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

void ui_ctrl_reply_show_end(const char *text)
{
    bsp_display_lock(0);
    // Not scrolled with the speech: the timer is stopped and the end of the text is shown
    lv_timer_pause(scroll_timer_handle);
    reply_content_get = false;
    lv_label_set_text(ui_LabelReplyContent, text);
    lv_obj_update_layout(ui_ContainerReplyContent);
    lv_coord_t extra = lv_obj_get_self_height(ui_LabelReplyContent) - lv_obj_get_height(ui_ContainerReplyContent);
    lv_obj_scroll_to_y(ui_ContainerReplyContent, extra > 0 ? extra : 0, LV_ANIM_OFF);
    bsp_display_unlock();
}

/*
 * The question above the answer always scrolls horizontally: a short one gets spaces after it until it is wider
 * than the label (the circular scroll only runs for a text wider than its label)
 */
static void question_set_text(const char *text)
{
    const lv_font_t *font = lv_obj_get_style_text_font(ui_LabelReplyQuestion, LV_PART_MAIN);
    lv_point_t size;
    lv_txt_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_coord_t width = lv_obj_get_style_width(ui_LabelReplyQuestion, LV_PART_MAIN);   // set (205), even before a layout
    lv_coord_t space = lv_font_get_glyph_width(font, ' ', ' ');
    size_t spaces = (size.x > width || space <= 0) ? 0 : (size_t)((width - size.x) / space + 2);
    char *padded = malloc(strlen(text) + spaces + 1);
    if (!padded) {
        lv_label_set_text(ui_LabelReplyQuestion, text);
        return;
    }
    strcpy(padded, text);
    memset(padded + strlen(text), ' ', spaces);
    padded[strlen(text) + spaces] = '\0';
    lv_label_set_text(ui_LabelReplyQuestion, padded);
    free(padded);
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
            question_set_text(text);
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
    bsp_display_lock(0);
    lv_img_set_src(ui_ImageReplyLogo, logo_image(logo));
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
    s_manual_idle_us = esp_timer_get_time();
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
        bool done;
        if (s_manual_scroll) {
            // A list not read: left where the finger puts it; ends a while after the last move (not while touched)
            scroll_arrow_update();
            if (s_reply_pressed || s_reply_scrolling) {
                s_manual_idle_us = esp_timer_get_time();
            }
            done = reply_audio_end && esp_timer_get_time() - s_manual_idle_us > REPLY_MANUAL_IDLE_MS * 1000LL;
        } else {
            if (settings_get_parameter()->scroll_smooth) {
                reply_scroll_follow(visible, max_offset);
            } else {
                reply_scroll_by_line(visible, max_offset);
            }
            done = lv_obj_get_scroll_y(ui_ContainerReplyContent) >= max_offset && reply_audio_end;
        }

        if (done) {
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
