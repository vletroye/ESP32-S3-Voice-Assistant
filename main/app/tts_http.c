/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Speech services asked by one HTTP request per sentence: see tts_http.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "file_costs.h"
#include "llm_sse.h"
#include "tts_backend.h"
#include "tts_http.h"

#define TTS_HTTP_TIMEOUT_MS     15000
#define TTS_HTTP_READ_CHUNK     4096
/* A connection unused for this long is likely closed by the server: opened again while the user speaks */
#define TTS_HTTP_WARM_US        (30 * 1000000LL)
/* Bytes of a frame cut between two blocks, kept before the next one */
#define TTS_HTTP_FRAME_MAX      8

static void drop_client(tts_http_t *h)
{
    if (h->client) {
        esp_http_client_close(h->client);
        esp_http_client_cleanup(h->client);
        h->client = NULL;
    }
}

static esp_http_client_handle_t get_client(tts_http_t *h)
{
    const char *url = h->url;
    if (h->url_of) {
        free(h->url_made);
        h->url_made = h->url_of();
        url = h->url_made;
    }
    if (!url) {
        return NULL;
    }
    if (!h->client) {
        esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = TTS_HTTP_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = true,
        };
        h->client = esp_http_client_init(&config);
        if (h->client) {
            esp_http_client_set_header(h->client, "Content-Type", "application/json");
            for (const char *const *header = h->headers; header && header[0]; header += 2) {
                esp_http_client_set_header(h->client, header[0], header[1]);
            }
        }
    }
    if (h->client) {
        // The voice (in the URL) or the key may have changed: same host, the connection is kept
        esp_http_client_set_url(h->client, url);
        const char *key = tts_backend_settings()->api_key;
        free(h->key_value);
        h->key_value = NULL;
        asprintf(&h->key_value, h->key_format, key ? key : "");
        esp_http_client_set_header(h->client, h->key_header, h->key_value ? h->key_value : "");
    }
    return h->client;
}

/* One request: the bytes of audio received, how it ended ("completed", "failed", "cut"...) */
static size_t request_once(tts_http_t *h, const char *text, const char *body, uint32_t generation, bool warm_up,
                           const tts_http_reader_t *reader, const char **status_out, bool *failed_out)
{
    int64_t start_us = esp_timer_get_time();
    const char *status_text = "completed";
    bool failed = false;
    int status = -1;
    size_t received = 0;
    for (int attempt = 0; body && attempt < 2; attempt++) {
        esp_http_client_handle_t client = get_client(h);
        size_t len = strlen(body);
        if (client && esp_http_client_open(client, len) == ESP_OK && esp_http_client_write(client, body, len) == (int)len &&
                esp_http_client_fetch_headers(client) >= 0) {
            status = esp_http_client_get_status_code(client);
            break;
        }
        drop_client(h);     // a kept-alive connection gone stale: once more on a fresh one
    }
    // The block read (PSRAM), after the bytes of a frame cut by the previous one (all services: the synthesis task)
    static char *chunk = NULL;
    if (!chunk) {
        chunk = heap_caps_malloc(TTS_HTTP_FRAME_MAX + TTS_HTTP_READ_CHUNK + 1, MALLOC_CAP_SPIRAM);
    }
    int n;
    if (!chunk) {
        status_text = "no memory";
        failed = true;
    } else if (status != 200) {
        // The error said by the service ({"detail": {"status", "message"}}, {"err_code", "err_msg"}...)
        llm_buf_t err = {0};
        while (h->client && status > 0 && (n = esp_http_client_read(h->client, chunk, TTS_HTTP_READ_CHUNK)) > 0) {
            llm_buf_append(&err, chunk, n);
        }
        ESP_LOGE(h->tag, "HTTP %d: %.300s", status, err.len ? err.p : "(no answer)");
        if (status == 402 || tts_backend_is_no_credit(err.p)) {
            tts_backend_no_credit();
        }
        llm_buf_free(&err);
        drop_client(h);
        status_text = "failed";
        failed = (status <= 0);
    } else {
        unsigned frame = h->frame_bytes > 1 && h->frame_bytes <= TTS_HTTP_FRAME_MAX ? h->frame_bytes : 1;
        size_t carry = 0;
        int64_t first_us = 0;
        while (!(reader->done && *reader->done) &&
                (n = esp_http_client_read(h->client, chunk + carry, TTS_HTTP_READ_CHUNK)) > 0) {
            if (!warm_up && generation != tts_backend_generation()) {
                status_text = "cancelled";
                break;
            }
            if (!first_us) {
                first_us = esp_timer_get_time();
            }
            received += n;
            // Whole frames only: a frame cut between two blocks waits for the next one
            size_t bytes = carry + n;
            size_t whole = bytes - bytes % frame;
            if (whole) {
                reader->on_bytes(reader->ctx, chunk, whole);
            }
            carry = bytes - whole;
            memmove(chunk, chunk + whole, carry);
        }
        int64_t now = esp_timer_get_time();
        ESP_LOGI(h->tag, "%s%u KB of audio received in %lld ms (first bytes after %lld ms)", warm_up ? "warm-up: " : "",
                 (unsigned)(received / 1024), (now - start_us) / 1000, first_us ? (first_us - start_us) / 1000 : -1LL);
        bool done = reader->done && *reader->done;
        if (reader->error && reader->error[0]) {
            ESP_LOGE(h->tag, "error: %s", reader->error);
            if (tts_backend_is_no_credit(reader->error)) {
                tts_backend_no_credit();
            }
            status_text = "failed";
        } else if (!done && strcmp(status_text, "cancelled") != 0 &&
                   !esp_http_client_is_complete_data_received(h->client)) {
            status_text = "cut";
            failed = true;
        }
        if (strcmp(status_text, "completed") == 0) {
            // A stream over before the end of its body ([DONE]): the rest read, the connection reused
            while (esp_http_client_read(h->client, chunk, TTS_HTTP_READ_CHUNK) > 0) {
            }
        }
        if (strcmp(status_text, "completed") != 0 || !esp_http_client_is_complete_data_received(h->client)) {
            drop_client(h);
        }
        // Billed by the character: a token a character
        costs_add_tokens(COST_TTS, tts_backend_settings()->model, (int)strlen(text), 0, 0, 0);
        h->last_used_us = esp_timer_get_time();
    }
    *status_out = status_text;
    *failed_out = failed;
    return received;
}

void tts_http_request(tts_http_t *h, const char *text, char *body, uint32_t generation, bool warm_up,
                      const tts_http_reader_t *reader)
{
    if (!warm_up) {
        tts_backend_sentence_start(generation);
    }
    const char *status_text = "completed";
    bool failed = false;
    size_t received = request_once(h, text, body, generation, warm_up, reader, &status_text, &failed);
    if (!warm_up && received == 0 && strcmp(status_text, "completed") == 0 && generation == tts_backend_generation()) {
        // Answered without any audio (Mistral, now and then): the sentence would not be said; asked again once, on a
        // fresh connection
        ESP_LOGW(h->tag, "No audio for \"%.40s\": asked again", text);
        drop_client(h);
        received = request_once(h, text, body, generation, warm_up, reader, &status_text, &failed);
        if (received == 0 && strcmp(status_text, "completed") == 0) {
            status_text = "no audio";
            failed = true;      // a sentence lost: the reply is said to have been heard badly
        }
    }
    if (!warm_up) {
        tts_backend_sentence_end(status_text, failed);
    }
    free(body);
}

void tts_http_prewarm(tts_http_t *h, void (*warm_up)(void))
{
    if (h->client && esp_timer_get_time() - h->last_used_us < TTS_HTTP_WARM_US) {
        return;
    }
    int64_t start_us = esp_timer_get_time();
    warm_up();
    ESP_LOGI(h->tag, "warmed up in %lld ms", (esp_timer_get_time() - start_us) / 1000);
}

void tts_http_stop(tts_http_t *h)
{
    drop_client(h);
}
