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
 * OpenRouter has its own). The connection is kept between sentences (keep-alive). Billed by the character:
 * counted as a token a character.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_audio_simple_dec.h"
#include "esp_mp3_dec.h"
#include "cJSON.h"
#include "file_costs.h"
#include "llm_sse.h"
#include "tts_stream.h"
#include "tts_backend.h"

#define OPENROUTER_TTS_URL          "https://openrouter.ai/api/v1/audio/speech"
#define OPENROUTER_TTS_TIMEOUT_MS   15000
#define OPENROUTER_TTS_READ_CHUNK   4096
#define OPENROUTER_TTS_IN_MAX       (16 * 1024)     /* MP3 bytes waiting to be decoded */
/* A connection unused for this long is likely closed by the server: opened again while the user speaks */
#define OPENROUTER_TTS_WARM_US      (30 * 1000000LL)

static const char *TAG = "openrouter_tts";

static esp_http_client_handle_t s_client = NULL;
static char *s_auth = NULL;
static int64_t s_last_used_us = 0;

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
    size_t received;            /* MP3 bytes, for the log */
    int64_t first_us;           /* first bytes of audio received */
} stream_t;

static void drop_client(void)
{
    if (s_client) {
        esp_http_client_close(s_client);
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
}

static esp_http_client_handle_t get_client(void)
{
    if (!s_client) {
        esp_http_client_config_t config = {
            .url = OPENROUTER_TTS_URL,
            .method = HTTP_METHOD_POST,
            .timeout_ms = OPENROUTER_TTS_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = true,
        };
        s_client = esp_http_client_init(&config);
        if (s_client) {
            esp_http_client_set_header(s_client, "Content-Type", "application/json");
            esp_http_client_set_header(s_client, "HTTP-Referer", "https://github.com/vletroye/ESP32-S3-Voice-Assistant");
            esp_http_client_set_header(s_client, "X-Title", "ESP32-S3 Voice Assistant");
        }
    }
    if (s_client) {
        free(s_auth);
        s_auth = NULL;
        asprintf(&s_auth, "Bearer %s", tts_backend_settings()->api_key ? tts_backend_settings()->api_key : "");
        esp_http_client_set_header(s_client, "Authorization", s_auth);
    }
    return s_client;
}

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

/* MP3 bytes received: decoded as far as they go */
static void feed(stream_t *st, const char *bytes, size_t len)
{
    st->received += len;
    if (st->warm_up || !st->dec) {
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
    drop_client();
    ESP_LOGI(TAG, "OpenRouter speech closed");
}

static void openrouter_upkeep(void)
{
}

/* A sentence, or with warm_up a request whose audio is dropped (nothing reported to the speech engine) */
static void request(const char *text, uint32_t generation, bool warm_up)
{
    if (!warm_up) {
        tts_backend_sentence_start(generation);
    }
    stream_t st = {
        .generation = generation,
        .warm_up = warm_up,
    };
    char *body = build_request(text);
    int64_t start_us = esp_timer_get_time();
    const char *status_text = "completed";
    bool failed = false;
    int status = -1;
    for (int attempt = 0; body && attempt < 2; attempt++) {
        esp_http_client_handle_t client = get_client();
        size_t len = strlen(body);
        if (client && esp_http_client_open(client, len) == ESP_OK && esp_http_client_write(client, body, len) == (int)len &&
                esp_http_client_fetch_headers(client) >= 0) {
            status = esp_http_client_get_status_code(client);
            break;
        }
        drop_client();      // a kept-alive connection gone stale: once more on a fresh one
    }
    static char *chunk = NULL;     // the block read (PSRAM)
    if (!chunk) {
        chunk = heap_caps_malloc(OPENROUTER_TTS_READ_CHUNK + 1, MALLOC_CAP_SPIRAM);
    }
    int n;
    if (!chunk) {
        status_text = "no memory";
        failed = true;
    } else if (status != 200) {
        // {"error": {"code": 402, "message": "..."}}
        llm_buf_t err = {0};
        while (s_client && status > 0 && (n = esp_http_client_read(s_client, chunk, OPENROUTER_TTS_READ_CHUNK)) > 0) {
            llm_buf_append(&err, chunk, n);
        }
        ESP_LOGE(TAG, "HTTP %d: %.300s", status, err.len ? err.p : "(no answer)");
        if (status == 402 || tts_backend_is_no_credit(err.p)) {
            tts_backend_no_credit();
        }
        llm_buf_free(&err);
        drop_client();
        status_text = "failed";
        failed = (status <= 0);
    } else {
        if (!warm_up) {
            esp_audio_simple_dec_cfg_t cfg = { .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3 };
            st.in = heap_caps_malloc(OPENROUTER_TTS_IN_MAX, MALLOC_CAP_SPIRAM);
            st.out_size = 4608 * 2;     // an MP3 frame of 1152 stereo samples; grown when needed
            st.out = heap_caps_malloc(st.out_size, MALLOC_CAP_SPIRAM);
            if (!st.in || !st.out || esp_audio_simple_dec_open(&cfg, &st.dec) != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "Could not open the MP3 decoder");
                st.dec = NULL;
            }
        }
        while ((n = esp_http_client_read(s_client, chunk, OPENROUTER_TTS_READ_CHUNK)) > 0) {
            if (!warm_up && generation != tts_backend_generation()) {
                status_text = "cancelled";
                break;
            }
            if (!st.first_us) {
                st.first_us = esp_timer_get_time();
            }
            feed(&st, chunk, n);
        }
        int64_t now = esp_timer_get_time();
        ESP_LOGI(TAG, "%s%u KB of MP3 received in %lld ms (first bytes after %lld ms)", warm_up ? "warm-up: " : "",
                 (unsigned)(st.received / 1024), (now - start_us) / 1000,
                 st.first_us ? (st.first_us - start_us) / 1000 : -1LL);
        bool complete = esp_http_client_is_complete_data_received(s_client);
        if (strcmp(status_text, "cancelled") != 0 && !complete) {
            status_text = "cut";
            failed = true;
        }
        if (!complete) {
            drop_client();
        }
        // Billed by the character: a token a character
        costs_add_tokens(COST_TTS, tts_backend_settings()->model, (int)strlen(text), 0, 0, 0);
        s_last_used_us = esp_timer_get_time();
    }
    if (!warm_up) {
        tts_backend_sentence_end(status_text, failed);
    }
    if (st.dec) {
        esp_audio_simple_dec_close(st.dec);
    }
    free(st.in);
    free(st.out);
    free(st.mono);
    free(st.res);
    free(body);
}

static void openrouter_say(const char *text, uint32_t generation)
{
    request(text, generation, false);
}

/* The user is speaking: a connection unused for a while is opened again (TLS handshake) with a "." not played */
static void openrouter_prewarm(void)
{
    if (s_client && esp_timer_get_time() - s_last_used_us < OPENROUTER_TTS_WARM_US) {
        return;
    }
    int64_t start_us = esp_timer_get_time();
    request(".", 0, true);
    ESP_LOGI(TAG, "warmed up in %lld ms", (esp_timer_get_time() - start_us) / 1000);
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
