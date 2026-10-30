/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * OpenRouter client: Chat Completions API (OpenAI-compatible) with server-sent events, the conversation kept
 * on the device and sent with each request. One key gives access to the models of many providers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "llm.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "ai_provider_openrouter.h"
#include "file_costs.h"
#include "llm_sse.h"

#define OPENROUTER_URL              "https://openrouter.ai/api/v1/chat/completions"
/* A tiny free answer (the key's limits): opens the TLS connection in advance */
#define OPENROUTER_PREWARM_URL      "https://openrouter.ai/api/v1/key"
/* Shown on openrouter.ai for the requests of this app */
#define OPENROUTER_REFERER          "https://github.com/vletroye/ESP32-S3-Voice-Assistant"
#define OPENROUTER_TITLE            "ESP32-S3-BOX-3 Voice Assistant"
/* Counts the reasoning tokens too: room for a short spoken answer after a little reasoning */
#define OPENROUTER_MAX_TOKENS       2048
#define OPENROUTER_TIMEOUT_MS       30000
#define OPENROUTER_READ_CHUNK       512
/* History kept for follow-up questions, cut in one go to keep its start (and the providers' caches) stable */
#define OPENROUTER_HISTORY_MAX_MSGS 24
#define OPENROUTER_HISTORY_TRIM_TO  12

static const char *TAG = "openrouter";

static char *s_auth = NULL;             /* "Bearer <key>" */
static char *s_model = NULL;
static char *s_instructions = NULL;
static char *s_effort = NULL;
/* Conversation history: [{role: "user" | "assistant", content}] (the instructions are added per request) */
static cJSON *s_messages = NULL;
/* Kept between requests so that the TLS connection can be reused (keep-alive) */
static esp_http_client_handle_t s_client = NULL;

typedef struct {
    llm_text_cb_t on_text;
    void *ctx;
    llm_sse_t sse;
    llm_buf_t text;             /* whole answer text */
    char finish[24];            /* finish_reason: "stop", "length", "content_filter", "error"... */
    bool done;                  /* [DONE] or an error event received */
    llm_error_t error;

    int64_t start_us;
    int64_t first_text_us;
    int input_tokens;
    int cached_tokens;
    int output_tokens;
    int reasoning_tokens;
} stream_t;

static void replace_str(char **dst, const char *src)
{
    free(*dst);
    *dst = (src && src[0]) ? strdup(src) : NULL;
}

esp_err_t openrouter_chat_init(const char *api_key, const char *model, const char *instructions, const char *effort)
{
    ESP_RETURN_ON_FALSE(api_key && api_key[0] && model && model[0], ESP_ERR_INVALID_ARG, TAG,
                        "API key and model required");
    replace_str(&s_model, model);
    free(s_auth);
    s_auth = malloc(strlen(api_key) + 8);
    ESP_RETURN_ON_FALSE(s_auth, ESP_ERR_NO_MEM, TAG, "no memory");
    sprintf(s_auth, "Bearer %s", api_key);
    replace_str(&s_instructions, instructions);
    replace_str(&s_effort, effort);
    openrouter_chat_reset();
    ESP_LOGI(TAG, "model: %s, reasoning effort: %s, streaming", s_model, s_effort ? s_effort : "(default)");
    return ESP_OK;
}

void openrouter_chat_set_instructions(const char *instructions)
{
    replace_str(&s_instructions, instructions);
}

void openrouter_chat_reset(void)
{
    cJSON_Delete(s_messages);
    s_messages = cJSON_CreateArray();
}

/* Map an API error to the error kinds shown to the user */
static llm_error_t classify_error(int code, const char *message)
{
    // First: 402, "Insufficient credits" (also said by the provider behind it with another code)
    if (code == 402 || (message && strcasestr(message, "credit"))) {
        return LLM_ERR_CREDIT;
    }
    if (code == 401 || code == 403) {
        return LLM_ERR_AUTH;
    }
    if (code == 408 || code == 429 || code >= 500) {
        return LLM_ERR_BUSY;
    }
    return LLM_ERR_OTHER;
}

/* {"error": {"code", "message", "metadata"}} (HTTP errors and errors in the stream) */
static llm_error_t error_from_json(int status, const cJSON *json)
{
    const cJSON *api_error = cJSON_GetObjectItem(json, "error");
    const cJSON *code = cJSON_GetObjectItem(api_error, "code");
    const cJSON *message = cJSON_GetObjectItem(api_error, "message");
    int code_num = cJSON_IsNumber(code) ? code->valueint : status;
    ESP_LOGE(TAG, "HTTP %d: %d: %s", status, code_num, cJSON_IsString(message) ? message->valuestring : "?");
    return classify_error(code_num, cJSON_IsString(message) ? message->valuestring : NULL);
}

static char *build_request(void)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", s_model);
    cJSON_AddBoolToObject(req, "stream", true);
    cJSON_AddNumberToObject(req, "max_tokens", OPENROUTER_MAX_TOKENS);
    if (s_effort) {
        // Reasoning kept short and not returned (the answer is spoken); ignored by models that do not reason
        cJSON *reasoning = cJSON_AddObjectToObject(req, "reasoning");
        cJSON_AddStringToObject(reasoning, "effort", s_effort);
        cJSON_AddBoolToObject(reasoning, "exclude", true);
    }
    cJSON_AddItemToObject(cJSON_AddObjectToObject(req, "usage"), "include", cJSON_CreateTrue());
    /* The instructions, then the history (referenced, not copied: it stays owned by s_messages) */
    cJSON *messages = cJSON_AddArrayToObject(req, "messages");
    if (s_instructions) {
        cJSON *system = cJSON_CreateObject();
        cJSON_AddStringToObject(system, "role", "system");
        cJSON_AddStringToObject(system, "content", s_instructions);
        cJSON_AddItemToArray(messages, system);
    }
    const cJSON *msg;
    cJSON_ArrayForEach(msg, s_messages) {
        cJSON_AddItemReferenceToArray(messages, (cJSON *)msg);
    }
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return body;
}

/* ---------- SSE event handling ---------- */

static void sse_dispatch(const char *data, void *ctx)
{
    stream_t *st = (stream_t *)ctx;
    if (strcmp(data, "[DONE]") == 0) {
        st->done = true;
        return;
    }
    cJSON *json = cJSON_Parse(data);
    if (!json) {
        ESP_LOGW(TAG, "Unparsable event: %.120s", data);
        return;
    }
    if (cJSON_IsObject(cJSON_GetObjectItem(json, "error"))) {
        st->error = error_from_json(200, json);
        st->done = true;
        cJSON_Delete(json);
        return;
    }
    const cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "choices"), 0);
    const cJSON *content = cJSON_GetObjectItem(cJSON_GetObjectItem(choice, "delta"), "content");
    if (cJSON_IsString(content) && content->valuestring[0]) {
        llm_buf_append(&st->text, content->valuestring, strlen(content->valuestring));
        if (!st->first_text_us) {
            st->first_text_us = esp_timer_get_time();
        }
        if (st->on_text) {
            st->on_text(content->valuestring, st->ctx);
        }
    }
    const cJSON *finish = cJSON_GetObjectItem(choice, "finish_reason");
    if (cJSON_IsString(finish)) {
        strlcpy(st->finish, finish->valuestring, sizeof(st->finish));
    }
    const cJSON *usage = cJSON_GetObjectItem(json, "usage");
    const cJSON *v;
    if ((v = cJSON_GetObjectItem(usage, "prompt_tokens")) && cJSON_IsNumber(v)) {
        st->input_tokens = v->valueint;
    }
    if ((v = cJSON_GetObjectItem(cJSON_GetObjectItem(usage, "prompt_tokens_details"), "cached_tokens")) &&
            cJSON_IsNumber(v)) {
        st->cached_tokens = v->valueint;
    }
    if ((v = cJSON_GetObjectItem(usage, "completion_tokens")) && cJSON_IsNumber(v)) {
        st->output_tokens = v->valueint;
    }
    if ((v = cJSON_GetObjectItem(cJSON_GetObjectItem(usage, "completion_tokens_details"), "reasoning_tokens")) &&
            cJSON_IsNumber(v)) {
        st->reasoning_tokens = v->valueint;
    }
    cJSON_Delete(json);
}

/* ---------- HTTP ---------- */

static esp_http_client_handle_t get_client(void)
{
    if (!s_client) {
        esp_http_client_config_t config = {
            .url = OPENROUTER_URL,
            .method = HTTP_METHOD_POST,
            .timeout_ms = OPENROUTER_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = true,
        };
        s_client = esp_http_client_init(&config);
        if (s_client) {
            esp_http_client_set_header(s_client, "Content-Type", "application/json");
            esp_http_client_set_header(s_client, "Accept", "text/event-stream");
            esp_http_client_set_header(s_client, "HTTP-Referer", OPENROUTER_REFERER);
            esp_http_client_set_header(s_client, "X-Title", OPENROUTER_TITLE);
        }
    }
    if (s_client) {
        esp_http_client_set_header(s_client, "Authorization", s_auth);
    }
    return s_client;
}

static void drop_client(void)
{
    if (s_client) {
        esp_http_client_close(s_client);
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
}

/* Open the request and send the body; retried once on a fresh connection if a kept-alive one went stale */
static esp_err_t send_request(const char *body, int *status)
{
    size_t len = strlen(body);
    for (int attempt = 0; attempt < 2; attempt++) {
        esp_http_client_handle_t client = get_client();
        ESP_RETURN_ON_FALSE(client, ESP_ERR_NO_MEM, TAG, "Failed to init http client");
        if (esp_http_client_open(client, len) == ESP_OK &&
                esp_http_client_write(client, body, len) == (int)len &&
                esp_http_client_fetch_headers(client) >= 0) {
            *status = esp_http_client_get_status_code(client);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Request failed (attempt %d), reconnecting", attempt + 1);
        drop_client();
    }
    return ESP_FAIL;
}

void openrouter_chat_prewarm(void)
{
    if (!s_auth) {
        return;
    }
    int64_t t0 = esp_timer_get_time();
    esp_http_client_handle_t client = get_client();
    if (!client) {
        return;
    }
    esp_http_client_set_url(client, OPENROUTER_PREWARM_URL);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    int status = -1;
    if (esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
        status = esp_http_client_get_status_code(client);
        /* Read the body to its end, so that it is not read as the start of the next response */
        char discard[256];
        while (esp_http_client_read(client, discard, sizeof(discard)) > 0) {
        }
    }
    if (status == 200 && esp_http_client_is_complete_data_received(client)) {
        esp_http_client_set_url(client, OPENROUTER_URL);
        esp_http_client_set_method(client, HTTP_METHOD_POST);
    } else {
        drop_client();
    }
    ESP_LOGI(TAG, "prewarm: HTTP %d in %lld ms", status, (esp_timer_get_time() - t0) / 1000);
}

/* Keep the history short; older exchanges are dropped in user/assistant pairs */
static void trim_history(void)
{
    if (cJSON_GetArraySize(s_messages) <= OPENROUTER_HISTORY_MAX_MSGS) {
        return;
    }
    while (cJSON_GetArraySize(s_messages) > OPENROUTER_HISTORY_TRIM_TO) {
        cJSON_DeleteItemFromArray(s_messages, 0);
        cJSON_DeleteItemFromArray(s_messages, 0);
    }
}

const cJSON *openrouter_chat_history(void)
{
    return s_messages;
}

void openrouter_chat_set_history(cJSON *messages)
{
    cJSON_Delete(s_messages);
    s_messages = messages;
    trim_history();
}

static void add_message(const char *role, const char *text)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", role);
    cJSON_AddStringToObject(msg, "content", text);
    cJSON_AddItemToArray(s_messages, msg);
}

llm_error_t openrouter_chat_stream(const char *text, llm_text_cb_t on_text, void *ctx, char **full_text)
{
    if (full_text) {
        *full_text = NULL;
    }
    if (!s_auth || !s_messages) {
        return LLM_ERR_NO_KEY;
    }

    stream_t st = {
        .on_text = on_text,
        .ctx = ctx,
        .sse = { .on_event = sse_dispatch },
        .error = LLM_OK,
        .start_us = esp_timer_get_time(),
    };
    st.sse.ctx = &st;
    int status = 0;
    char *body = NULL;

    /* Append the question; removed again if the exchange fails */
    add_message("user", text);

    body = build_request();
    if (!body) {
        st.error = LLM_ERR_OTHER;
        goto end;
    }
    if (send_request(body, &status) != ESP_OK) {
        st.error = LLM_ERR_NETWORK;
        goto end;
    }

    char chunk[OPENROUTER_READ_CHUNK];
    int n;
    if (status != 200) {
        /* Errors before streaming are a plain JSON body */
        llm_buf_t err_body = {0};
        while ((n = esp_http_client_read(s_client, chunk, sizeof(chunk))) > 0) {
            llm_buf_append(&err_body, chunk, n);
        }
        cJSON *json = err_body.len ? cJSON_Parse(err_body.p) : NULL;
        if (json) {
            st.error = error_from_json(status, json);
            cJSON_Delete(json);
        } else {
            ESP_LOGE(TAG, "HTTP %d: %.300s", status, err_body.len ? err_body.p : "(empty)");
            st.error = classify_error(status, NULL);
        }
        llm_buf_free(&err_body);
        drop_client();
        goto end;
    }

    while (!st.done && !llm_cancelled() && (n = esp_http_client_read(s_client, chunk, sizeof(chunk))) > 0) {
        llm_sse_feed(&st.sse, chunk, n);
    }
    if (st.error == LLM_OK && !st.done) {
        ESP_LOGE(TAG, "Stream ended before [DONE]");
        st.error = LLM_ERR_NETWORK;
    }
    if (st.error == LLM_OK) {
        /* Read the end of the chunked body so that the connection can be reused for the next question */
        while (esp_http_client_read(s_client, chunk, sizeof(chunk)) > 0) {
        }
    }
    if (st.error != LLM_OK || !esp_http_client_is_complete_data_received(s_client)) {
        /* The connection is not in a reusable state */
        drop_client();
    }

    if (st.error == LLM_OK) {
        if (strcmp(st.finish, "content_filter") == 0 && st.text.len == 0) {
            ESP_LOGW(TAG, "Request declined (content_filter)");
            st.error = LLM_ERR_REFUSAL;
        } else if (strcmp(st.finish, "length") == 0) {
            ESP_LOGW(TAG, "Answer truncated (length)");
        }
        if (st.error == LLM_OK && st.text.len == 0) {
            ESP_LOGE(TAG, "No text in answer (finish: %s)", st.finish[0] ? st.finish : "-");
            st.error = LLM_ERR_OTHER;
        }
    }

    int64_t now = esp_timer_get_time();
    ESP_LOGI(TAG, "first text after %lld ms, total %lld ms, finish: %s, tokens in %d (cached %d), out %d "
             "(reasoning %d)", st.first_text_us ? (st.first_text_us - st.start_us) / 1000 : -1LL,
             (now - st.start_us) / 1000, st.finish[0] ? st.finish : "-", st.input_tokens, st.cached_tokens,
             st.output_tokens, st.reasoning_tokens);
    // completion_tokens counts the reasoning too
    llm_last_stats = (llm_stats_t) {
        st.first_text_us ? (int)((st.first_text_us - st.start_us) / 1000) : -1, (int)((now - st.start_us) / 1000),
        st.output_tokens - st.reasoning_tokens, st.reasoning_tokens
    };
    // prompt_tokens counts the cached ones; completion_tokens the reasoning (billed too)
    costs_add_tokens(COST_LLM, s_model, st.input_tokens - st.cached_tokens, st.cached_tokens, 0, st.output_tokens);

end:
    if (st.error == LLM_OK) {
        add_message("assistant", st.text.p);
        trim_history();
        if (full_text) {
            *full_text = st.text.p;
            st.text.p = NULL;
        }
    } else {
        /* Drop the unanswered question so that the history keeps alternating user/assistant */
        cJSON_DeleteItemFromArray(s_messages, cJSON_GetArraySize(s_messages) - 1);
    }
    llm_sse_free(&st.sse);
    llm_buf_free(&st.text);
    free(body);
    return st.error;
}
