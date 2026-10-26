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
 * @brief Set the OpenAI Realtime speech parameters. The strings are copied.
 *
 * @param base_url     OpenAI base URL ending with '/', e.g. "https://api.openai.com/v1/" (wss:// is derived)
 * @param api_key      OpenAI key
 * @param model        e.g. "gpt-realtime-2.1-mini"
 * @param voice        e.g. "marin"
 * @param instructions Reading instructions (read verbatim, language, tone), may be NULL
 */
void tts_stream_configure(const char *base_url, const char *api_key, const char *model, const char *voice,
                          const char *instructions);

/**
 * @brief Change the voice (e.g. "cedar"). A Realtime session keeps its voice once it has spoken, so the
 *        synthesis reconnects with a new session; the next sentences use the new voice.
 */
void tts_stream_set_voice(const char *voice);

/** Change the reading instructions (language, tone): a new session too, before the next sentence */
void tts_stream_set_instructions(const char *instructions);

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
 * @brief Stop the current reply now (queued sentences and buffered audio are dropped).
 */
void tts_stream_cancel(void);

/**
 * @brief True when nothing has been sent to the speaker for at least this long (the I2S output has drained):
 *        the codec registers can then be written without crackles.
 */
bool tts_stream_is_silent(uint32_t ms);

/**
 * @brief True when the reply just played was cut several times in the middle of a sentence, or lost
 *        sentences: the speech service (or the connection to it) was slower than the speech.
 */
bool tts_stream_reply_was_choppy(void);

#ifdef __cplusplus
}
#endif
