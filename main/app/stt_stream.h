/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Called with the partial transcript so far (only valid during the call), from the WebSocket task.
 */
typedef void (*stt_partial_cb_t)(const char *text);

/**
 * @brief Create the streaming task and the audio buffer (PSRAM). Call once.
 */
esp_err_t stt_stream_init(void);

/**
 * @brief Set the OpenAI Realtime transcription parameters. The strings are copied.
 *
 * @param api_key  OpenAI key
 * @param model    e.g. "gpt-live-transcribe"
 * @param language ISO-639-1 code, e.g. "fr"
 */
void stt_stream_configure(const char *api_key, const char *model, const char *language);

/** Change the transcription language (ISO-639-1): the session is updated before the next question */
void stt_stream_set_language(const char *language);

/**
 * @brief Start a transcription session: connect, then stream the audio passed to stt_stream_write().
 *
 * Audio written before the connection is up is buffered (up to 10 s), so nothing is lost.
 */
void stt_stream_start(stt_partial_cb_t on_partial);

/**
 * @brief Feed 16 kHz mono 16-bit audio (the AFE output). Ignored when no session is running.
 */
void stt_stream_write(const int16_t *samples, size_t count);

/**
 * @brief End of speech: send the remaining audio, commit and wait for the final transcript.
 *
 * @return The transcript (to be freed with free()), or NULL on error or timeout
 */
char *stt_stream_finish(uint32_t timeout_ms);

/**
 * @brief True if the transcription of the current session already produced some text.
 */
bool stt_stream_has_text(void);

/** The last transcription refused for lack of OpenAI credit (insufficient_quota) */
bool stt_stream_no_credit(void);

/**
 * @brief Wait until the session is connected (or failed / ended).
 *
 * @return true if connected
 */
bool stt_stream_wait_connected(uint32_t timeout_ms);

/**
 * @brief Drop the session (nobody spoke): nothing is transcribed.
 */
void stt_stream_abort(void);

#ifdef __cplusplus
}
#endif
