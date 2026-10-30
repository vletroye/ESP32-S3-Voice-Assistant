/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Settings applied at run time, without restarting (voice commands, Settings screen). The caller saves them
 * to NVS.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Switch the assistant to another language: texts and screens, clock, model instructions,
 *        transcription and voice (the speech session reconnects, ~2 s, before the next sentence).
 */
void assistant_set_language(const char *code);

/**
 * @brief Answer with another model from now on (its saved conversation is restored if any, else a new one).
 */
void assistant_set_model(const char *model);

/**
 * @brief Use this transcription / speech model ([stt] / [tts] of AGENTS.INI), NULL for the one of the settings;
 *        applied at once (a new connection), not saved
 */
void assistant_set_stt(const char *model);
void assistant_set_tts(const char *model);

/** Provider of the speech in use ("openai", "mistral"): whose voices "change de voix" offers */
const char *assistant_tts_provider(void);

/**
 * @brief Where the user lives (2-letter country code, town; NULL keeps the current value): web search and
 *        model instructions updated at once.
 */
void assistant_set_location(const char *country, const char *city);

/**
 * @brief Same as assistant_set_language() / assistant_set_model(), applied by the main task within a second:
 *        for callers whose stack is small (display task), as the model change reads and writes files.
 */
void assistant_request_language(const char *code);
void assistant_request_model(const char *model);

/**
 * @brief Icons of the Configuration screen, done by the main task:
 *        - soft reset: settings back to their defaults (Wi-Fi, API keys, Homey, Persona and Memory kept), phrase
 *          files and AGENTS.INI replaced by the firmware's, then restart;
 *        - factory reset: CONFIG.INI saved to CONFIG.BKP (if it was changed), everything erased (settings, files,
 *          conversation), then configuration mode with a PC connected, otherwise restart.
 */
void assistant_request_restore(bool factory);

/** Restart in configuration mode, done by the main task (conversation, volume and costs saved first) */
void assistant_request_config(void);

/** Double tap while "J'écoute" / "Je réfléchis": dropped, back to sleep (from the LVGL task) */
void assistant_cancel(void);

/** Model test: show a text as the answer (not spoken), scrolled to its last line */
void assistant_show(const char *text);

/**
 * @brief Model test: one spoken announcement made of several texts, queued as they come (the screen unchanged,
 *        no listening afterwards): begin, then a text per result (returns at once, spoken after the previous
 *        ones), then end, which returns once everything is spoken (or the test stopped).
 */
void assistant_announce_begin(void);
void assistant_announce(const char *text);
void assistant_announce_end(void);

/** Show and speak a text as an answer, then listen for the user's answer (follow-up listening) */
void assistant_say(const char *text);

/** Model test finished: back to sleep */
void assistant_test_done(void);

#ifdef __cplusplus
}
#endif
