/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_CTRL_PANEL_SLEEP = 0,
    UI_CTRL_PANEL_LISTEN,
    UI_CTRL_PANEL_GET,
    UI_CTRL_PANEL_REPLY,
} ui_ctrl_panel_t;

typedef enum {
    UI_CTRL_LABEL_LISTEN_SPEAK = 0,
    UI_CTRL_LABEL_REPLY_QUESTION,
    UI_CTRL_LABEL_REPLY_CONTENT,
} ui_ctrl_label_t;

/* Icon at the left of the answer: the model's provider, or the web when it searched */
typedef enum {
    UI_CTRL_LOGO_CLAUDE,
    UI_CTRL_LOGO_OPENAI,
    UI_CTRL_LOGO_GEMINI,
    UI_CTRL_LOGO_OPENROUTER,
    UI_CTRL_LOGO_XAI,
    UI_CTRL_LOGO_WEB,
    UI_CTRL_LOGO_LOCAL,     /* answered by the device without a model (memory commands, errors) */
} ui_ctrl_logo_t;

void ui_ctrl_reply_set_logo(ui_ctrl_logo_t logo);

/** Texts of the screens and day names of the clock in the current language (after a change at run time) */
void ui_ctrl_apply_language(void);

/** The list of models changed (free OpenRouter models read): the Settings screen shows the new one */
void ui_ctrl_models_changed(void);

/** The assistant sleeps on its clock (not the Settings or welcome screen): the screen may go off */
bool ui_ctrl_screen_can_sleep(void);

/** Leave the listening / answer panels (sleep) and show the Settings screen */
void ui_ctrl_open_settings(void);

/** A message over every screen (no button): before a restart */
void ui_ctrl_show_message(const char *text);

/** Problems of a language file: a screen with the text (scrolled when long), "Skip" (back) and "Config"
 *  (configuration mode, to correct the file) */
void ui_ctrl_show_problems(const char *title, const char *text);

/** Show or hide the time and the date of the sleep screen */
void ui_ctrl_show_clock(bool shown);

/** Settings screen selections (language, model) set back to the stored settings */
void ui_ctrl_settings_sync(void);

void ui_ctrl_init(void);

void ui_ctrl_show_panel(ui_ctrl_panel_t panel, uint16_t timeout);

/** The sleep panel is shown (the assistant is idle) */
bool ui_ctrl_is_asleep(void);

void ui_ctrl_label_show_text(ui_ctrl_label_t label, const char *text);

void ui_sleep_show_animation(void);

void ui_ctrl_reply_set_audio_start_flag(bool result);

bool ui_ctrl_reply_get_audio_start_flag(void);

void ui_ctrl_reply_set_audio_end_flag(bool result);

/** Add a note on a line of its own after the answer shown */
void ui_ctrl_reply_append_notice(const char *notice);

/** Show a text as the answer, scrolled to its end (its last line visible), without following the speech */
void ui_ctrl_reply_show_end(const char *text);

void ui_ctrl_guide_jump(void);

#ifdef __cplusplus
}
#endif
