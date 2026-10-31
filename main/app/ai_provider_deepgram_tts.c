/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Deepgram speech (Aura-2): one HTTP request per sentence (tts_http.c), its raw audio read as it comes,
 *
 *   POST https://api.deepgram.com/v1/speak?model=aura-2-agathe-fr&encoding=mulaw&sample_rate=16000&container=none
 *   (header "Authorization: Token <key>") {"text": "..."} -> 16 kHz 8-bit mu-law mono samples
 *
 * Mu-law at the 16 kHz of the speaker: 16 KB a second of speech. Deepgram's servers are far (~90 ms round trip)
 * and the TCP window of the device small (5760 bytes): about 26 KB/s come, less than the 48 KB/s of 16-bit
 * audio at 24 kHz, which was played with gaps.
 *
 * The voice is the model of the request (aura-2-<name>-<language>, the voice id of [voices] in AGENTS.INI); the
 * model of [tts] ("aura-2") only names the service and its price. Billed by the character.
 */

#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "tts_stream.h"
#include "tts_backend.h"
#include "tts_http.h"

#define DEEPGRAM_TTS_URL        "https://api.deepgram.com/v1/speak?encoding=mulaw&sample_rate=16000&container=none"
#define DEEPGRAM_TTS_VOICE      "aura-2-thalia-en"
#define DEEPGRAM_TTS_SAMPLES    4096    /* a block of the answer: a byte a sample */

static const char *TAG = "deepgram_tts";

/* The voice is in the URL */
static char *url_of(void)
{
    const char *voice = tts_backend_settings()->voice;
    char *url = NULL;
    asprintf(&url, DEEPGRAM_TTS_URL "&model=%s", voice ? voice : DEEPGRAM_TTS_VOICE);
    return url;
}

static tts_http_t s_http = {
    .tag = "deepgram_tts",
    .url_of = url_of,
    .key_header = "Authorization",
    .key_format = "Token %s",
};

/* G.711 mu-law byte -> 16-bit sample */
static int16_t mulaw_decode(uint8_t u)
{
    u = ~u;
    int t = (((u & 0x0F) << 3) + 0x84) << ((u & 0x70) >> 4);
    return (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
}

/* A byte a sample, decoded to 16 bits (PSRAM) */
static void on_bytes(void *ctx, char *bytes, size_t len)
{
    bool warm_up = *(bool *)ctx;
    static int16_t *samples = NULL;
    if (!samples) {
        samples = heap_caps_malloc(DEEPGRAM_TTS_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    }
    if (warm_up || !samples) {
        return;
    }
    for (size_t i = 0; i < len && i < DEEPGRAM_TTS_SAMPLES; i++) {
        samples[i] = mulaw_decode((uint8_t)bytes[i]);
    }
    tts_backend_audio_16k(samples, len < DEEPGRAM_TTS_SAMPLES ? len : DEEPGRAM_TTS_SAMPLES);
}

/* A sentence, or with warm_up a request whose audio is dropped */
static void request(const char *text, uint32_t generation, bool warm_up)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "text", text);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    tts_http_reader_t reader = { .on_bytes = on_bytes, .ctx = &warm_up };
    tts_http_request(&s_http, text, body, generation, warm_up, &reader);
}

static void deepgram_start(void)
{
}

static void deepgram_stop(void)
{
    tts_http_stop(&s_http);
    ESP_LOGI(TAG, "Deepgram speech closed");
}

static void deepgram_upkeep(void)
{
}

static void deepgram_say(const char *text, uint32_t generation)
{
    request(text, generation, false);
}

static void warm_up(void)
{
    request(".", 0, true);
}

static void deepgram_prewarm(void)
{
    tts_http_prewarm(&s_http, warm_up);
}

const tts_backend_t tts_backend_deepgram = {
    .name = "deepgram",
    .start = deepgram_start,
    .stop = deepgram_stop,
    .upkeep = deepgram_upkeep,
    .say = deepgram_say,
    .prewarm = deepgram_prewarm,
    // A request per sentence, its audio streamed: the defaults (to be tuned once measured)
    .join_chars = 0,
    .reserve_ms = 0,
};
