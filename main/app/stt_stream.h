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

/* The transcription services (ai_provider_<name>_stt.c) */
typedef struct stt_backend stt_backend_t;
extern const stt_backend_t stt_backend_openai;
extern const stt_backend_t stt_backend_mistral;
extern const stt_backend_t stt_backend_deepgram;
extern const stt_backend_t stt_backend_groq;

/**
 * @brief Set the transcription service and its parameters (the strings are copied). Called again to change the
 *        service or the model at run time: a new connection.
 *
 * @param backend  &stt_backend_openai, &stt_backend_mistral, &stt_backend_deepgram
 * @param api_key  Key of the service
 * @param model    e.g. "gpt-live-transcribe", "voxtral-mini-transcribe-realtime-3-0"
 * @param language ISO-639-1 code, e.g. "fr" (NULL: unchanged)
 */
void stt_stream_configure(const stt_backend_t *backend, const char *api_key, const char *model,
                          const char *language);

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

/**
 * @brief Model test: transcribe this audio (16 kHz mono, at most 10 s) with the service configured.
 *
 * @param ms Time from the commit (all the audio sent) to the transcript (-1 if none)
 * @return The transcript (to be freed with free()), or NULL
 */
char *stt_stream_test(const int16_t *audio, size_t samples, int *ms);

/** The last transcription refused for lack of credit (insufficient_quota, 402...) */
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
