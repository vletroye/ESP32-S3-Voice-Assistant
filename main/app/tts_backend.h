/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * A speech service (ai_provider_<name>_tts.c) for tts_stream.c: it turns a sentence into 24 kHz audio. tts_stream.c
 * keeps the sentences of the reply, the resampling to 16 kHz and the playback; the service gives the audio of a
 * sentence with tts_backend_audio(), between tts_backend_sentence_start() and tts_backend_sentence_end().
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tts_backend {
    const char *name;           /* "openai", "mistral" (logs) */
    /* Chosen: connect in advance if it keeps a connection (synthesis task) */
    void (*start)(void);
    /* Another service chosen: close and free what it holds (synthesis task) */
    void (*stop)(void);
    /* Between sentences, about every 200 ms: keep the connection up, apply new settings (synthesis task) */
    void (*upkeep)(void);
    /* Speak a sentence of this generation (blocking until its audio is given, or the reply cancelled) */
    void (*say)(const char *text, uint32_t generation);
};

/* What the service is to use, set by tts_stream_configure() / tts_stream_set_voice() / ..._set_instructions() */
typedef struct {
    const char *base_url;       /* OpenAI's ("https://api.openai.com/v1/") */
    const char *api_key;
    const char *model;
    const char *voice;
    const char *instructions;   /* reading instructions (OpenAI), may be NULL */
    uint32_t version;           /* incremented at each change: the service applies them when it differs */
} tts_settings_t;

/** The current settings (read from the synthesis task) */
const tts_settings_t *tts_backend_settings(void);

/** The current reply: a sentence of an older generation is dropped (the reply was cancelled) */
uint32_t tts_backend_generation(void);

/** A sentence starts (its audio follows) */
void tts_backend_sentence_start(uint32_t generation);

/** Audio of the sentence: 24 kHz, 16-bit signed, mono, little-endian (any length, even odd) */
void tts_backend_audio(const uint8_t *pcm, size_t len);

/** The sentence is over: status for the log ("completed", "failed"...); failed: sentence lost */
void tts_backend_sentence_end(const char *status, bool failed);

/** The speech was refused for lack of credit (said on screen after the reply) */
void tts_backend_no_credit(void);

/** An error text saying the credit is exhausted ("insufficient_quota", "quota", "billing", "credit") */
bool tts_backend_is_no_credit(const char *text);

#ifdef __cplusplus
}
#endif
