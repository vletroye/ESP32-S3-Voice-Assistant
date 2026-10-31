/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ElevenLabs speech (Flash v2.5): one HTTP request per sentence (tts_http.c), its raw audio read as it comes,
 *
 *   POST https://api.elevenlabs.io/v1/text-to-speech/<voice id>/stream?output_format=pcm_16000
 *   (header "xi-api-key: <key>") {"text": "...", "model_id": "eleven_flash_v2_5"} -> 16 kHz 16-bit mono samples
 *
 * At the 16 kHz of the speaker: played as they are. The voice is one of the account's (its id in [voices] of
 * AGENTS.INI); without one for the language in use, the first ElevenLabs voice of [voices] (the model speaks 32
 * languages). Billed in credits by the character: counted as a token a character.
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"
#include "llm.h"
#include "tts_stream.h"
#include "tts_backend.h"
#include "tts_http.h"

#define ELEVENLABS_TTS_URL  "https://api.elevenlabs.io/v1/text-to-speech/%s/stream?output_format=pcm_16000"

static const char *TAG = "elevenlabs_tts";

/* The voice of the settings, else the first ElevenLabs voice of [voices] */
static const char *voice_id(void)
{
    const char *voice = tts_backend_settings()->voice;
    for (size_t i = 0; !voice && i < llm_voice_count(); i++) {
        const llm_voice_t *v = llm_voice(i);
        if (strcmp(v->provider, "elevenlabs") == 0) {
            voice = v->id;
        }
    }
    return voice ? voice : "";
}

/* The voice is in the URL */
static char *url_of(void)
{
    char *url = NULL;
    asprintf(&url, ELEVENLABS_TTS_URL, voice_id());
    return url;
}

static tts_http_t s_http = {
    .tag = "elevenlabs_tts",
    .url_of = url_of,
    .key_header = "xi-api-key",
    .key_format = "%s",
    .frame_bytes = 2,       // 16-bit samples: a byte cut between two blocks waits for the next one
};

static void on_bytes(void *ctx, char *bytes, size_t len)
{
    if (!*(bool *)ctx) {
        tts_backend_audio_16k((const int16_t *)bytes, len / 2);
    }
}

/* A sentence, or with warm_up a request whose audio is dropped */
static void request(const char *text, uint32_t generation, bool warm_up)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "text", text);
    cJSON_AddStringToObject(req, "model_id", tts_backend_settings()->model);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    tts_http_reader_t reader = { .on_bytes = on_bytes, .ctx = &warm_up };
    tts_http_request(&s_http, text, body, generation, warm_up, &reader);
}

static void elevenlabs_start(void)
{
}

static void elevenlabs_stop(void)
{
    tts_http_stop(&s_http);
    ESP_LOGI(TAG, "ElevenLabs speech closed");
}

static void elevenlabs_upkeep(void)
{
}

static void elevenlabs_say(const char *text, uint32_t generation)
{
    request(text, generation, false);
}

static void warm_up(void)
{
    request(".", 0, true);
}

static void elevenlabs_prewarm(void)
{
    tts_http_prewarm(&s_http, warm_up);
}

const tts_backend_t tts_backend_elevenlabs = {
    .name = "elevenlabs",
    .start = elevenlabs_start,
    .stop = elevenlabs_stop,
    .upkeep = elevenlabs_upkeep,
    .say = elevenlabs_say,
    .prewarm = elevenlabs_prewarm,
    // A request per sentence, its audio streamed: the defaults (to be tuned once measured)
    .join_chars = 0,
    .reserve_ms = 0,
};
