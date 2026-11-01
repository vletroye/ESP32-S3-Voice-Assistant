/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Helpers shared by the chat clients: see llm_common.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "llm_common.h"

#define LLM_HTTP_TIMEOUT_MS     30000
#define LLM_HTTP_READ_CHUNK     512

/* ---------- strings ---------- */

void llm_str_set(char **dst, const char *src)
{
    free(*dst);
    *dst = (src && src[0]) ? strdup(src) : NULL;
}

bool llm_bearer_set(char **dst, const char *api_key)
{
    free(*dst);
    *dst = NULL;
    return asprintf(dst, "Bearer %s", api_key ? api_key : "") >= 0;
}

/* ---------- conversation history ---------- */

void llm_history_reset(cJSON **messages)
{
    cJSON_Delete(*messages);
    *messages = cJSON_CreateArray();
}

void llm_history_set(cJSON **messages, cJSON *taken)
{
    cJSON_Delete(*messages);
    *messages = taken;
    llm_history_trim(*messages, NULL);
}

void llm_history_trim(cJSON *messages, bool (*cannot_start)(const cJSON *message))
{
    int keep = cJSON_GetArraySize(messages) > LLM_HISTORY_MAX_MSGS ? LLM_HISTORY_TRIM_TO : cJSON_GetArraySize(messages);
    while (cJSON_GetArraySize(messages) > keep ||
            (cannot_start && cJSON_GetArraySize(messages) > 0 && cannot_start(cJSON_GetArrayItem(messages, 0)))) {
        cJSON_DeleteItemFromArray(messages, 0);
        cJSON_DeleteItemFromArray(messages, 0);
    }
}

void llm_history_add(cJSON *messages, const char *role, const char *text)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", role);
    cJSON_AddStringToObject(msg, "content", text);
    cJSON_AddItemToArray(messages, msg);
}

void llm_history_drop_from(cJSON *messages, int count)
{
    while (cJSON_GetArraySize(messages) > count) {
        cJSON_DeleteItemFromArray(messages, cJSON_GetArraySize(messages) - 1);
    }
}

/* ---------- the answer being streamed ---------- */

void llm_answer_begin(llm_answer_t *a, llm_text_cb_t on_text, void *ctx)
{
    *a = (llm_answer_t) {
        .on_text = on_text, .ctx = ctx, .start_us = esp_timer_get_time()
    };
}

void llm_answer_add(llm_answer_t *a, const char *text, size_t len)
{
    if (len == 0) {
        return;
    }
    llm_buf_append(&a->text, text, len);
    if (!a->first_text_us) {
        a->first_text_us = esp_timer_get_time();
    }
    if (!a->on_text) {
        return;
    }
    if (text[len] == '\0') {
        a->on_text(text, a->ctx);
    } else {
        char *piece = strndup(text, len);
        if (piece) {
            a->on_text(piece, a->ctx);
            free(piece);
        }
    }
}

int llm_answer_first_ms(const llm_answer_t *a)
{
    return a->first_text_us ? (int)((a->first_text_us - a->start_us) / 1000) : -1;
}

int llm_answer_total_ms(const llm_answer_t *a)
{
    return (int)((esp_timer_get_time() - a->start_us) / 1000);
}

void llm_answer_stats(const llm_answer_t *a, int answer_tokens, int reasoning_tokens)
{
    llm_last_stats = (llm_stats_t) {
        llm_answer_first_ms(a), llm_answer_total_ms(a), answer_tokens, reasoning_tokens
    };
}

/* ---------- HTTPS connection ---------- */

esp_http_client_handle_t llm_http_client(llm_http_t *h)
{
    if (!h->client) {
        esp_http_client_config_t config = {
            .url = h->url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = LLM_HTTP_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = true,
        };
        h->client = esp_http_client_init(&config);
        if (h->client) {
            esp_http_client_set_header(h->client, "Content-Type", "application/json");
            esp_http_client_set_header(h->client, "Accept", "text/event-stream");
            for (const char *const *header = h->headers; header && header[0]; header += 2) {
                esp_http_client_set_header(h->client, header[0], header[1]);
            }
        }
    }
    if (h->client) {
        // The key may have changed (another model of the same provider), and so the URL (Gemini, Mistral)
        esp_http_client_set_header(h->client, h->key_header, h->key ? h->key : "");
        esp_http_client_set_url(h->client, h->url);
        esp_http_client_set_method(h->client, HTTP_METHOD_POST);
    }
    return h->client;
}

void llm_http_drop(llm_http_t *h)
{
    if (h->client) {
        esp_http_client_close(h->client);
        esp_http_client_cleanup(h->client);
        h->client = NULL;
    }
}

esp_err_t llm_http_send(llm_http_t *h, const char *body, int *status)
{
    size_t len = strlen(body);
    for (int attempt = 0; attempt < 2; attempt++) {
        esp_http_client_handle_t client = llm_http_client(h);
        if (!client) {
            ESP_LOGE(h->tag, "Failed to init http client");
            return ESP_ERR_NO_MEM;
        }
        // A reasoning model may stay silent for long before its answer
        esp_http_client_set_timeout_ms(client, llm_answer_timeout_ms());
        if (esp_http_client_open(client, len) == ESP_OK &&
                esp_http_client_write(client, body, len) == (int)len &&
                esp_http_client_fetch_headers(client) >= 0) {
            *status = esp_http_client_get_status_code(client);
            return ESP_OK;
        }
        ESP_LOGW(h->tag, "Request failed (attempt %d), reconnecting", attempt + 1);
        llm_http_drop(h);
    }
    return ESP_FAIL;
}

int llm_http_prewarm(llm_http_t *h, const char *url, llm_buf_t *body, size_t max)
{
    if (!h->key) {
        return -1;
    }
    int64_t t0 = esp_timer_get_time();
    esp_http_client_handle_t client = llm_http_client(h);
    if (!client) {
        return -1;
    }
    esp_http_client_set_url(client, url);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    int status = -1;
    if (esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
        status = esp_http_client_get_status_code(client);
        /*
         * Read the body to its end: esp_http_client_flush_response() can leave a small body that came with the
         * headers in the connection, where it would be read as the start of the next response
         */
        char chunk[256];
        int n;
        while ((n = esp_http_client_read(client, chunk, sizeof(chunk))) > 0) {
            if (body && body->len < max) {
                llm_buf_append(body, chunk, n);
            }
        }
    }
    if (status == 200 && esp_http_client_is_complete_data_received(client)) {
        esp_http_client_set_url(client, h->url);
        esp_http_client_set_method(client, HTTP_METHOD_POST);
    } else {
        llm_http_drop(h);
    }
    ESP_LOGI(h->tag, "prewarm: HTTP %d in %lld ms", status, (esp_timer_get_time() - t0) / 1000);
    return status;
}

llm_error_t llm_http_error(llm_http_t *h, int status, llm_error_parse_t parse, const void *ctx, llm_buf_t *body)
{
    // Errors before streaming are a plain JSON body
    llm_buf_t err_body = {0};
    char chunk[LLM_HTTP_READ_CHUNK];
    int n;
    while (h->client && (n = esp_http_client_read(h->client, chunk, sizeof(chunk))) > 0) {
        llm_buf_append(&err_body, chunk, n);
    }
    cJSON *json = err_body.len ? cJSON_Parse(err_body.p) : NULL;
    if (!json) {
        ESP_LOGE(h->tag, "HTTP %d: %.300s", status, err_body.len ? err_body.p : "(empty)");
    }
    llm_error_t error = parse(ctx, status, json);
    cJSON_Delete(json);
    llm_http_drop(h);
    if (body) {
        *body = err_body;
    } else {
        llm_buf_free(&err_body);
    }
    return error;
}

void llm_http_stream(llm_http_t *h, llm_sse_t *sse, const bool *done)
{
    char chunk[LLM_HTTP_READ_CHUNK];
    int n;
    while (!(done && *done) && !llm_cancelled() && (n = esp_http_client_read(h->client, chunk, sizeof(chunk))) > 0) {
        llm_sse_feed(sse, chunk, n);
    }
}

void llm_http_finish(llm_http_t *h, bool ok)
{
    if (ok) {
        // Read the end of the chunked body so that the connection can be reused for the next question
        char chunk[LLM_HTTP_READ_CHUNK];
        while (esp_http_client_read(h->client, chunk, sizeof(chunk)) > 0) {
        }
    }
    if (!ok || !esp_http_client_is_complete_data_received(h->client)) {
        llm_http_drop(h);       // the connection is not in a reusable state
    }
}
