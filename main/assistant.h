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

#ifdef __cplusplus
}
#endif
