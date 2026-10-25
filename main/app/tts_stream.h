/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TTS_EVENT_FIRST_AUDIO,  /* the first samples of the reply reach the speaker */
    TTS_EVENT_DONE,         /* every sentence has been played (not sent after tts_stream_cancel()) */
} tts_event_t;

/** Called from the playback task */
typedef void (*tts_event_cb_t)(tts_event_t event);

/**
 * @brief Create the synthesis and playback tasks and the audio buffer (PSRAM). Call once.
 */
esp_err_t tts_stream_init(void);

/**
 * @brief Set the OpenAI speech parameters. The strings are copied.
 *
 * @param base_url     OpenAI base URL ending with '/', e.g. "https://api.openai.com/v1/"
 * @param api_key      OpenAI key
 * @param model        e.g. "gpt-4o-mini-tts"
 * @param voice        e.g. "marin"
 * @param instructions Speaking style (language, tone), may be NULL
 */
void tts_stream_configure(const char *base_url, const char *api_key, const char *model, const char *voice,
                          const char *instructions);

/**
 * @brief Start a new spoken reply; cancels the previous one if still playing.
 */
void tts_stream_begin(tts_event_cb_t cb);

/**
 * @brief Queue a sentence of the current reply (copied).
 */
void tts_stream_say(const char *sentence);

/**
 * @brief No more sentences for the current reply: TTS_EVENT_DONE follows once everything is played.
 */
void tts_stream_end(void);

/**
 * @brief Open the HTTPS connection in advance (asynchronous), while the user speaks.
 */
void tts_stream_prewarm(void);

/**
 * @brief Stop the current reply now (queued sentences and buffered audio are dropped).
 */
void tts_stream_cancel(void);

#ifdef __cplusplus
}
#endif
