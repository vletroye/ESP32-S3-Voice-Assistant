/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ElevenLabs speech (Flash v2.5): one HTTP request per sentence, its raw audio read as it comes,
 *
 *   POST https://api.elevenlabs.io/v1/text-to-speech/<voice id>/stream?output_format=pcm_16000
 *   (header "xi-api-key: <key>") {"text": "...", "model_id": "eleven_flash_v2_5"} -> 16 kHz 16-bit mono samples
 *
 * At the 16 kHz of the speaker: played as they are. The voice is one of the account's (its id in [voices] of
 * AGENTS.INI); without one for the language in use, the first ElevenLabs voice of [voices] (the model speaks 32
 * languages). The connection is kept between sentences (keep-alive). Billed in credits by the character: counted as a
 * token a character.
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
#include "llm.h"
#include "llm_sse.h"
#include "tts_stream.h"
#include "tts_backend.h"

#define ELEVENLABS_TTS_URL          "https://api.elevenlabs.io/v1/text-to-speech/%s/stream?output_format=pcm_16000"
#define ELEVENLABS_TTS_TIMEOUT_MS   15000
#define ELEVENLABS_TTS_READ_CHUNK   4096
/* A connection unused for this long is likely closed by the server: opened again while the user speaks */
#define ELEVENLABS_TTS_WARM_US      (30 * 1000000LL)

static const char *TAG = "elevenlabs_tts";

static esp_http_client_handle_t s_client = NULL;
static char *s_url = NULL;
static int64_t s_last_used_us = 0;

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
    asprintf(&s_url, ELEVENLABS_TTS_URL, voice_id());
    if (!s_url) {
        return NULL;
    }
    if (!s_client) {
        esp_http_client_config_t config = {
            .url = s_url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = ELEVENLABS_TTS_TIMEOUT_MS,
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
        esp_http_client_set_header(s_client, "xi-api-key", settings->api_key ? settings->api_key : "");
    }
    return s_client;
}

static void elevenlabs_start(void)
{
}

static void elevenlabs_stop(void)
{
    drop_client();
    ESP_LOGI(TAG, "ElevenLabs speech closed");
}

static void elevenlabs_upkeep(void)
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
    cJSON_AddStringToObject(req, "model_id", tts_backend_settings()->model);
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
    static char *chunk = NULL;     // the block read (PSRAM), with room for a byte left from the previous one
    if (!chunk) {
        chunk = heap_caps_malloc(ELEVENLABS_TTS_READ_CHUNK + 2, MALLOC_CAP_SPIRAM);
    }
    int n;
    if (!chunk) {
        status_text = "no memory";
        failed = true;
    } else if (status != 200) {
        // {"detail": {"status": "...", "message": "..."}}
        llm_buf_t err = {0};
        while (s_client && status > 0 && (n = esp_http_client_read(s_client, chunk, ELEVENLABS_TTS_READ_CHUNK)) > 0) {
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
        // 16-bit samples: a byte cut between two blocks waits for the next one
        size_t received = 0;
        int carry = 0;
        while ((n = esp_http_client_read(s_client, chunk + carry, ELEVENLABS_TTS_READ_CHUNK)) > 0) {
            if (!warm_up && generation != tts_backend_generation()) {
                status_text = "cancelled";
                break;
            }
            received += n;
            int bytes = carry + n;
            if (!warm_up) {
                tts_backend_audio_16k((const int16_t *)chunk, bytes / 2);
            }
            carry = bytes % 2;
            if (carry) {
                chunk[0] = chunk[bytes - 1];
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

static void elevenlabs_say(const char *text, uint32_t generation)
{
    request(text, generation, false);
}

/* The user is speaking: a connection unused for a while is opened again (TLS handshake) with a "." not played */
static void elevenlabs_prewarm(void)
{
    if (s_client && esp_timer_get_time() - s_last_used_us < ELEVENLABS_TTS_WARM_US) {
        return;
    }
    int64_t start_us = esp_timer_get_time();
    request(".", 0, true);
    ESP_LOGI(TAG, "warmed up in %lld ms", (esp_timer_get_time() - start_us) / 1000);
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
