/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Speech through OpenRouter (any model of its audio API: deepgram/aura-2, openai/gpt-4o-mini-tts...): one HTTP
 * request per sentence, its audio read as it comes,
 *
 *   POST https://openrouter.ai/api/v1/audio/speech {model, input, voice, response_format: "mp3"}
 *   (header "Authorization: Bearer <OpenRouter_key>") -> MP3 bytes
 *
 * MP3 rather than "pcm": a few KB a second instead of 48 (16-bit at 24 kHz), as the services behind OpenRouter
 * are far and the device receives slowly (see ai_provider_deepgram_tts.c). Decoded on the device (the MP3 decoder
 * of the radio), mixed to mono, then given at 24 kHz or 16 kHz to the speech engine, or brought to 16 kHz.
 *
 * The voices of [voices] in AGENTS.INI are those of the model (its id in the provider column: each model behind
 * OpenRouter has its own). The connection is kept between sentences (keep-alive, tts_http.c). Billed by the
 * character: counted as a token a character.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_audio_simple_dec.h"
#include "esp_mp3_dec.h"
#include "cJSON.h"
#include "tts_stream.h"
#include "tts_backend.h"
#include "tts_http.h"

#define OPENROUTER_TTS_IN_MAX       (16 * 1024)     /* MP3 bytes waiting to be decoded */

static const char *TAG = "openrouter_tts";

/* Shown on openrouter.ai for the requests of this app */
static const char *const s_headers[] = {
    "HTTP-Referer", "https://github.com/vletroye/ESP32-S3-Voice-Assistant",
    "X-Title", "ESP32-S3 Voice Assistant",
    NULL,
};

static tts_http_t s_http = {
    .tag = "openrouter_tts",
    .url = "https://openrouter.ai/api/v1/audio/speech",
    .key_header = "Authorization",
    .key_format = "Bearer %s",
    .headers = s_headers,
};

/* The audio of a sentence: MP3 waiting, the decoder, its output */
typedef struct {
    uint32_t generation;
    bool warm_up;               /* request of openrouter_prewarm(): its audio is not played */
    esp_audio_simple_dec_handle_t dec;
    uint8_t *in;
    size_t in_len;
    uint8_t *out;
    uint32_t out_size;
    int16_t *mono;
    size_t mono_cap;
    int16_t *res;               /* resampled to 16 kHz (another rate) */
    size_t res_cap;
    uint32_t rate;              /* of the decoded audio, 0 until known */
    double pos;                 /* resampling to 16 kHz (another rate): position in the input */
    int16_t last;               /* its last input sample */
    bool opened;                /* the decoder opened (at the first audio) */
} stream_t;

/* Decoded frames (16-bit, 1 or 2 channels) -> mono -> the speech engine, at its rate (24 or 16 kHz) or 16 kHz */
static void give_audio(stream_t *st, const int16_t *pcm, int frames, int channels)
{
    if ((size_t)frames > st->mono_cap) {
        int16_t *p = heap_caps_realloc(st->mono, frames * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!p) {
            return;
        }
        st->mono = p;
        st->mono_cap = frames;
    }
    for (int i = 0; i < frames; i++) {
        st->mono[i] = channels > 1 ? (int16_t)(((int32_t)pcm[2 * i] + pcm[2 * i + 1]) / 2) : pcm[i];
    }
    if (st->rate == 24000) {
        tts_backend_audio((const uint8_t *)st->mono, frames * sizeof(int16_t));
        return;
    }
    if (st->rate == 16000) {
        tts_backend_audio_16k(st->mono, frames);
        return;
    }
    // Another rate (22.05, 44.1, 48 kHz...): linear interpolation to 16 kHz; pos runs over the input, sample i
    // standing between mono[i - 1] (or the last of the previous block) and mono[i]
    double step = (double)st->rate / 16000.0;
    size_t needed = (size_t)(frames / step) + 2;
    if (needed > st->res_cap) {
        int16_t *p = heap_caps_realloc(st->res, needed * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!p) {
            return;
        }
        st->res = p;
        st->res_cap = needed;
    }
    size_t count = 0;
    while (st->pos < frames && count < st->res_cap) {
        int i = (int)st->pos;
        double frac = st->pos - i;
        int16_t a = i == 0 ? st->last : st->mono[i - 1];
        int16_t b = st->mono[i];
        st->res[count++] = (int16_t)(a + (b - a) * frac);
        st->pos += step;
    }
    st->pos -= frames;
    st->last = st->mono[frames - 1];
    tts_backend_audio_16k(st->res, count);
}

/* The MP3 decoder and its buffers, at the first audio of a sentence */
static void open_decoder(stream_t *st)
{
    st->opened = true;
    esp_audio_simple_dec_cfg_t cfg = { .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3 };
    st->in = heap_caps_malloc(OPENROUTER_TTS_IN_MAX, MALLOC_CAP_SPIRAM);
    st->out_size = 4608 * 2;    // an MP3 frame of 1152 stereo samples; grown when needed
    st->out = heap_caps_malloc(st->out_size, MALLOC_CAP_SPIRAM);
    if (!st->in || !st->out || esp_audio_simple_dec_open(&cfg, &st->dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Could not open the MP3 decoder");
        st->dec = NULL;
    }
}

/* MP3 bytes received: decoded as far as they go */
static void feed(void *ctx, char *bytes, size_t len)
{
    stream_t *st = ctx;
    if (st->warm_up) {
        return;
    }
    if (!st->opened) {
        open_decoder(st);
    }
    if (!st->dec) {
        return;
    }
    while (len > 0) {
        size_t take = OPENROUTER_TTS_IN_MAX - st->in_len < len ? OPENROUTER_TTS_IN_MAX - st->in_len : len;
        memcpy(st->in + st->in_len, bytes, take);
        st->in_len += take;
        bytes += take;
        len -= take;
        while (st->in_len > 0) {
            esp_audio_simple_dec_raw_t raw = { .buffer = st->in, .len = st->in_len };
            esp_audio_simple_dec_out_t frame = { .buffer = st->out, .len = st->out_size };
            esp_audio_err_t ret = esp_audio_simple_dec_process(st->dec, &raw, &frame);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                uint8_t *bigger = heap_caps_realloc(st->out, frame.needed_size, MALLOC_CAP_SPIRAM);
                if (!bigger) {
                    return;
                }
                st->out = bigger;
                st->out_size = frame.needed_size;
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK && ret != ESP_AUDIO_ERR_DATA_LACK) {
                raw.consumed = raw.consumed ? raw.consumed : 1;     // a bad byte skipped
            }
            if (raw.consumed) {
                memmove(st->in, st->in + raw.consumed, st->in_len - raw.consumed);
                st->in_len -= raw.consumed;
            }
            if (frame.decoded_size) {
                esp_audio_simple_dec_info_t info = { 0 };
                esp_audio_simple_dec_get_info(st->dec, &info);
                if (st->rate != info.sample_rate) {
                    st->rate = info.sample_rate;
                    ESP_LOGI(TAG, "audio: MP3, %" PRIu32 " Hz, %d channel(s), %" PRIu32 " bit/s", info.sample_rate,
                             info.channel, info.bitrate);
                }
                int channels = info.channel > 1 ? 2 : 1;
                give_audio(st, (const int16_t *)st->out, frame.decoded_size / (sizeof(int16_t) * channels), channels);
            } else if (!raw.consumed) {
                break;      // more bytes needed
            }
        }
    }
}

static char *build_request(const char *text)
{
    const tts_settings_t *settings = tts_backend_settings();
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", settings->model);
    cJSON_AddStringToObject(req, "input", text);
    if (settings->voice && settings->voice[0]) {
        cJSON_AddStringToObject(req, "voice", settings->voice);
    }
    cJSON_AddStringToObject(req, "response_format", "mp3");
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return body;
}

static void openrouter_start(void)
{
    esp_mp3_dec_register();     // already done by the radio: nothing more
}

static void openrouter_stop(void)
{
    tts_http_stop(&s_http);
    ESP_LOGI(TAG, "OpenRouter speech closed");
}

static void openrouter_upkeep(void)
{
}

/* A sentence, or with warm_up a request whose audio is dropped */
static void request(const char *text, uint32_t generation, bool warm_up)
{
    stream_t st = {
        .generation = generation,
        .warm_up = warm_up,
    };
    tts_http_reader_t reader = { .on_bytes = feed, .ctx = &st };
    tts_http_request(&s_http, text, build_request(text), generation, warm_up, &reader);
    if (st.dec) {
        esp_audio_simple_dec_close(st.dec);
    }
    free(st.in);
    free(st.out);
    free(st.mono);
    free(st.res);
}

static void openrouter_say(const char *text, uint32_t generation)
{
    request(text, generation, false);
}

static void warm_up(void)
{
    request(".", 0, true);
}

static void openrouter_prewarm(void)
{
    tts_http_prewarm(&s_http, warm_up);
}

const tts_backend_t tts_backend_openrouter = {
    .name = "openrouter",
    .start = openrouter_start,
    .stop = openrouter_stop,
    .upkeep = openrouter_upkeep,
    .say = openrouter_say,
    .prewarm = openrouter_prewarm,
    // As Mistral's (a request per sentence, from far): the sentences waiting joined, some audio in reserve (to be
    // tuned once measured)
    .join_chars = 600,
    .reserve_ms = 600,
};
