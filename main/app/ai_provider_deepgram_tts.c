/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Deepgram speech (Aura-2): one HTTP request per sentence, its raw audio read as it comes,
 *
 *   POST https://api.deepgram.com/v1/speak?model=aura-2-agathe-fr&encoding=mulaw&sample_rate=16000&container=none
 *   (header "Authorization: Token <key>") {"text": "..."} -> 16 kHz 8-bit mu-law mono samples
 *
 * Mu-law at the 16 kHz of the speaker: 16 KB a second of speech. Deepgram's servers are far (~90 ms round trip)
 * and the TCP window of the device small (5760 bytes): about 26 KB/s come, less than the 48 KB/s of 16-bit
 * audio at 24 kHz, which was played with gaps.
 *
 * The voice is the model of the request (aura-2-<name>-<language>, the voice id of [voices] in AGENTS.INI); the
 * model of [tts] ("aura-2") only names the service and its price. The connection is kept between sentences
 * (keep-alive). Billed by the character: counted as a token a character.
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "file_costs.h"
#include "llm_sse.h"
#include "tts_stream.h"
#include "tts_backend.h"

#define DEEPGRAM_TTS_URL        "https://api.deepgram.com/v1/speak?encoding=mulaw&sample_rate=16000&container=none"
#define DEEPGRAM_TTS_VOICE      "aura-2-thalia-en"
#define DEEPGRAM_TTS_TIMEOUT_MS 15000
#define DEEPGRAM_TTS_READ_CHUNK 4096
/* A connection unused for this long is likely closed by the server: opened again while the user speaks */
#define DEEPGRAM_TTS_WARM_US    (30 * 1000000LL)

static const char *TAG = "deepgram_tts";

static esp_http_client_handle_t s_client = NULL;
static char *s_auth = NULL;
static char *s_url = NULL;
/* Last request answered (esp_timer us) */
static int64_t s_last_used_us = 0;

/* G.711 mu-law byte -> 16-bit sample */
static int16_t mulaw_decode(uint8_t u)
{
    u = ~u;
    int t = (((u & 0x0F) << 3) + 0x84) << ((u & 0x70) >> 4);
    return (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
}

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
    const tts_settings_t *settings = tts_backend_settings();
    free(s_url);
    s_url = NULL;
    asprintf(&s_url, DEEPGRAM_TTS_URL "&model=%s", settings->voice ? settings->voice : DEEPGRAM_TTS_VOICE);
    if (!s_url) {
        return NULL;
    }
    if (!s_client) {
        esp_http_client_config_t config = {
            .url = s_url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = DEEPGRAM_TTS_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = true,
        };
        s_client = esp_http_client_init(&config);
        if (s_client) {
            esp_http_client_set_header(s_client, "Content-Type", "application/json");
        }
    }
    if (s_client) {
        // The voice may have changed: same host, the connection is kept
        esp_http_client_set_url(s_client, s_url);
        free(s_auth);
        s_auth = NULL;
        asprintf(&s_auth, "Token %s", settings->api_key ? settings->api_key : "");
        esp_http_client_set_header(s_client, "Authorization", s_auth);
    }
    return s_client;
}

static void deepgram_start(void)
{
}

static void deepgram_stop(void)
{
    drop_client();
    ESP_LOGI(TAG, "Deepgram speech closed");
}

static void deepgram_upkeep(void)
{
}

/* A sentence, or with warm_up a request whose audio is dropped (nothing reported to the speech engine) */
static void request(const char *text, uint32_t generation, bool warm_up)
{
    if (!warm_up) {
        tts_backend_sentence_start(generation);
    }
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "text", text);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
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
        chunk = heap_caps_malloc(DEEPGRAM_TTS_READ_CHUNK + 1, MALLOC_CAP_SPIRAM);
    }
    int n;
    if (!chunk) {
        status_text = "no memory";
        failed = true;
    } else if (status != 200) {
        // {"err_code": "...", "err_msg": "...", "request_id": "..."}
        llm_buf_t err = {0};
        while (s_client && status > 0 && (n = esp_http_client_read(s_client, chunk, DEEPGRAM_TTS_READ_CHUNK)) > 0) {
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
        // A byte a sample, decoded to 16 bits (PSRAM)
        static int16_t *samples = NULL;
        if (!samples) {
            samples = heap_caps_malloc(DEEPGRAM_TTS_READ_CHUNK * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        }
        size_t received = 0;
        while ((n = esp_http_client_read(s_client, chunk, DEEPGRAM_TTS_READ_CHUNK)) > 0) {
            if (!warm_up && generation != tts_backend_generation()) {
                status_text = "cancelled";
                break;
            }
            received += n;
            if (!warm_up && samples) {
                for (int i = 0; i < n; i++) {
                    samples[i] = mulaw_decode((uint8_t)chunk[i]);
                }
                tts_backend_audio_16k(samples, n);
            }
        }
        ESP_LOGI(TAG, "%s%u KB of audio received in %lld ms", warm_up ? "warm-up: " : "", (unsigned)(received / 1024),
                 (esp_timer_get_time() - start_us) / 1000);
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
    free(body);
}

static void deepgram_say(const char *text, uint32_t generation)
{
    request(text, generation, false);
}

/* The user is speaking: a connection unused for a while is opened again (TLS handshake) with a "." not played */
static void deepgram_prewarm(void)
{
    if (s_client && esp_timer_get_time() - s_last_used_us < DEEPGRAM_TTS_WARM_US) {
        return;
    }
    int64_t start_us = esp_timer_get_time();
    request(".", 0, true);
    ESP_LOGI(TAG, "warmed up in %lld ms", (esp_timer_get_time() - start_us) / 1000);
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
