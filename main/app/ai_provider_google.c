/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Gemini client: Gemini API generateContent with server-sent events (streamGenerateContent?alt=sse),
 * the conversation kept on the device and sent with each request.
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
#include "ai_provider_google.h"
#include "file_costs.h"
#include "llm_sse.h"

#define GEMINI_BASE_URL             "https://generativelanguage.googleapis.com/v1beta/models/"
/* Counts the thinking tokens too: room for a short spoken answer after a little thinking */
#define GEMINI_MAX_OUTPUT_TOKENS    2048
#define GEMINI_TIMEOUT_MS           30000
#define GEMINI_READ_CHUNK           512
/* History kept for follow-up questions, cut in one go to keep its start (and the implicit cache) stable */
#define GEMINI_HISTORY_MAX_MSGS     24
#define GEMINI_HISTORY_TRIM_TO      12
#define GEMINI_URL_SIZE             192

static const char *TAG = "gemini_chat";

static char s_url[GEMINI_URL_SIZE];
static char s_prewarm_url[GEMINI_URL_SIZE];
static char *s_key = NULL;
static char *s_model = NULL;
static char *s_instructions = NULL;
static char *s_thinking_level = NULL;
/* Conversation history: contents [{role: "user" | "model", parts: [{text}]}] */
static cJSON *s_messages = NULL;
/* Kept between requests so that the TLS connection can be reused (keep-alive) */
static esp_http_client_handle_t s_client = NULL;

typedef struct {
    llm_text_cb_t on_text;
    void *ctx;
    llm_sse_t sse;
    llm_buf_t text;             /* whole answer text */
    char *signature;            /* last thought signature, sent back with the answer */
    char finish[32];            /* finishReason: "STOP", "MAX_TOKENS", "SAFETY"... */
    bool blocked;               /* prompt blocked (promptFeedback.blockReason) */
    bool done;                  /* finishReason received */
    llm_error_t error;

    int64_t start_us;
    int64_t first_text_us;
    int input_tokens;
    int cached_tokens;
    int output_tokens;
    int thoughts_tokens;
} stream_t;

static void replace_str(char **dst, const char *src)
{
    free(*dst);
    *dst = (src && src[0]) ? strdup(src) : NULL;
}

esp_err_t gemini_chat_init(const char *api_key, const char *model, const char *instructions,
                           const char *thinking_level)
{
    ESP_RETURN_ON_FALSE(api_key && api_key[0] && model && model[0], ESP_ERR_INVALID_ARG, TAG,
                        "API key and model required");
    replace_str(&s_model, model);
    snprintf(s_url, sizeof(s_url), GEMINI_BASE_URL "%s:streamGenerateContent?alt=sse", s_model);
    snprintf(s_prewarm_url, sizeof(s_prewarm_url), GEMINI_BASE_URL "%s", s_model);
    replace_str(&s_key, api_key);
    replace_str(&s_instructions, instructions);
    replace_str(&s_thinking_level, thinking_level);
    gemini_chat_reset();
    ESP_LOGI(TAG, "model: %s, thinking level: %s, streaming", s_model,
             s_thinking_level ? s_thinking_level : "(default)");
    return ESP_OK;
}

void gemini_chat_set_instructions(const char *instructions)
{
    replace_str(&s_instructions, instructions);
}

void gemini_chat_reset(void)
{
    cJSON_Delete(s_messages);
    s_messages = cJSON_CreateArray();
}

/* Map an API error to the error kinds shown to the user */
static llm_error_t classify_error(int status, const char *api_status, const char *message)
{
    // First: prepaid credits depleted, billing not enabled (sometimes with PERMISSION_DENIED)
    if (message && (strcasestr(message, "billing") || strcasestr(message, "credit") ||
                    strcasestr(message, "prepay"))) {
        return LLM_ERR_CREDIT;
    }
    if (status == 401 || status == 403 || (message && strstr(message, "API key")) ||
            (api_status && (strcmp(api_status, "UNAUTHENTICATED") == 0 ||
                            strcmp(api_status, "PERMISSION_DENIED") == 0))) {
        return LLM_ERR_AUTH;
    }
    if (status == 429 || status >= 500 || (api_status && (strcmp(api_status, "RESOURCE_EXHAUSTED") == 0 ||
                                                           strcmp(api_status, "UNAVAILABLE") == 0))) {
        return LLM_ERR_BUSY;
    }
    return LLM_ERR_OTHER;
}

/* {"error": {"code", "message", "status"}} */
static llm_error_t error_from_json(int status, const cJSON *json)
{
    const cJSON *api_error = cJSON_GetObjectItem(json, "error");
    const cJSON *api_status = cJSON_GetObjectItem(api_error, "status");
    const cJSON *message = cJSON_GetObjectItem(api_error, "message");
    const char *status_str = cJSON_IsString(api_status) ? api_status->valuestring : NULL;
    const char *message_str = cJSON_IsString(message) ? message->valuestring : NULL;
    ESP_LOGE(TAG, "HTTP %d: %s: %s", status, status_str ? status_str : "?", message_str ? message_str : "?");
    return classify_error(status, status_str, message_str);
}

static char *build_request(void)
{
    cJSON *req = cJSON_CreateObject();
    if (s_instructions) {
        cJSON *system = cJSON_AddObjectToObject(req, "systemInstruction");
        cJSON *part = cJSON_CreateObject();
        cJSON_AddStringToObject(part, "text", s_instructions);
        cJSON_AddItemToObject(system, "parts", cJSON_CreateArray());
        cJSON_AddItemToArray(cJSON_GetObjectItem(system, "parts"), part);
    }
    /* Referenced, not copied: the history stays owned by s_messages */
    cJSON_AddItemReferenceToObject(req, "contents", s_messages);
    cJSON *config = cJSON_AddObjectToObject(req, "generationConfig");
    cJSON_AddNumberToObject(config, "maxOutputTokens", GEMINI_MAX_OUTPUT_TOKENS);
    if (s_thinking_level) {
        cJSON *thinking = cJSON_AddObjectToObject(config, "thinkingConfig");
        cJSON_AddStringToObject(thinking, "thinkingLevel", s_thinking_level);
    }
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return body;
}

/* ---------- SSE event handling ---------- */

/* Each event is a GenerateContentResponse: candidates[0].content.parts[] text, then finishReason and usage */
static void sse_dispatch(const char *data, void *ctx)
{
    stream_t *st = (stream_t *)ctx;
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
    const cJSON *feedback = cJSON_GetObjectItem(json, "promptFeedback");
    if (cJSON_IsString(cJSON_GetObjectItem(feedback, "blockReason"))) {
        st->blocked = true;
        st->done = true;
    }
    const cJSON *candidate = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "candidates"), 0);
    const cJSON *part;
    cJSON_ArrayForEach(part, cJSON_GetObjectItem(cJSON_GetObjectItem(candidate, "content"), "parts")) {
        const cJSON *signature = cJSON_GetObjectItem(part, "thoughtSignature");
        if (cJSON_IsString(signature)) {
            replace_str(&st->signature, signature->valuestring);
        }
        const cJSON *text = cJSON_GetObjectItem(part, "text");
        if (cJSON_IsTrue(cJSON_GetObjectItem(part, "thought")) || !cJSON_IsString(text) || !text->valuestring[0]) {
            continue;   // thought summaries are not spoken
        }
        llm_buf_append(&st->text, text->valuestring, strlen(text->valuestring));
        if (!st->first_text_us) {
            st->first_text_us = esp_timer_get_time();
        }
        if (st->on_text) {
            st->on_text(text->valuestring, st->ctx);
        }
    }
    const cJSON *finish = cJSON_GetObjectItem(candidate, "finishReason");
    if (cJSON_IsString(finish)) {
        strlcpy(st->finish, finish->valuestring, sizeof(st->finish));
        st->done = true;
    }
    const cJSON *usage = cJSON_GetObjectItem(json, "usageMetadata");
    const cJSON *v;
    if ((v = cJSON_GetObjectItem(usage, "promptTokenCount")) && cJSON_IsNumber(v)) {
        st->input_tokens = v->valueint;
    }
    if ((v = cJSON_GetObjectItem(usage, "cachedContentTokenCount")) && cJSON_IsNumber(v)) {
        st->cached_tokens = v->valueint;
    }
    if ((v = cJSON_GetObjectItem(usage, "candidatesTokenCount")) && cJSON_IsNumber(v)) {
        st->output_tokens = v->valueint;
    }
    if ((v = cJSON_GetObjectItem(usage, "thoughtsTokenCount")) && cJSON_IsNumber(v)) {
        st->thoughts_tokens = v->valueint;
    }
    cJSON_Delete(json);
}

/* ---------- HTTP ---------- */

static esp_http_client_handle_t get_client(void)
{
    if (!s_client) {
        esp_http_client_config_t config = {
            .url = s_url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = GEMINI_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = true,
        };
        s_client = esp_http_client_init(&config);
        if (s_client) {
            esp_http_client_set_header(s_client, "Content-Type", "application/json");
            esp_http_client_set_header(s_client, "Accept", "text/event-stream");
        }
    }
    if (s_client) {
        esp_http_client_set_header(s_client, "x-goog-api-key", s_key);
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

void gemini_chat_prewarm(void)
{
    if (!s_key) {
        return;
    }
    int64_t t0 = esp_timer_get_time();
    esp_http_client_handle_t client = get_client();
    if (!client) {
        return;
    }
    /* A free request with a tiny answer (the model's description): it opens the TLS connection */
    esp_http_client_set_url(client, s_prewarm_url);
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
        esp_http_client_set_url(client, s_url);
        esp_http_client_set_method(client, HTTP_METHOD_POST);
    } else {
        drop_client();
    }
    ESP_LOGI(TAG, "prewarm: HTTP %d in %lld ms", status, (esp_timer_get_time() - t0) / 1000);
}

/* Keep the history short; older exchanges are dropped in user/model pairs */
static void trim_history(void)
{
    if (cJSON_GetArraySize(s_messages) <= GEMINI_HISTORY_MAX_MSGS) {
        return;
    }
    while (cJSON_GetArraySize(s_messages) > GEMINI_HISTORY_TRIM_TO) {
        cJSON_DeleteItemFromArray(s_messages, 0);
        cJSON_DeleteItemFromArray(s_messages, 0);
    }
}

const cJSON *gemini_chat_history(void)
{
    return s_messages;
}

void gemini_chat_set_history(cJSON *messages)
{
    cJSON_Delete(s_messages);
    s_messages = messages;
    trim_history();
}

/* {"role": "user" | "model", "parts": [{"text": ..., "thoughtSignature": ...}]} */
static void add_message(const char *role, const char *text, const char *signature)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", role);
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", text);
    if (signature) {
        cJSON_AddStringToObject(part, "thoughtSignature", signature);
    }
    cJSON_AddItemToObject(msg, "parts", cJSON_CreateArray());
    cJSON_AddItemToArray(cJSON_GetObjectItem(msg, "parts"), part);
    cJSON_AddItemToArray(s_messages, msg);
}

llm_error_t gemini_chat_stream(const char *text, llm_text_cb_t on_text, void *ctx, char **full_text)
{
    if (full_text) {
        *full_text = NULL;
    }
    if (!s_key || !s_messages) {
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
    add_message("user", text, NULL);

    body = build_request();
    if (!body) {
        st.error = LLM_ERR_OTHER;
        goto end;
    }
    if (send_request(body, &status) != ESP_OK) {
        st.error = LLM_ERR_NETWORK;
        goto end;
    }

    char chunk[GEMINI_READ_CHUNK];
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
            st.error = classify_error(status, NULL, NULL);
        }
        llm_buf_free(&err_body);
        drop_client();
        goto end;
    }

    /* The last event carries finishReason; the body ends right after it */
    while (!llm_cancelled() && (n = esp_http_client_read(s_client, chunk, sizeof(chunk))) > 0) {
        llm_sse_feed(&st.sse, chunk, n);
    }
    if (st.error == LLM_OK && !st.done) {
        ESP_LOGE(TAG, "Stream ended before finishReason");
        st.error = LLM_ERR_NETWORK;
    }
    if (st.error != LLM_OK || !esp_http_client_is_complete_data_received(s_client)) {
        /* The connection is not in a reusable state */
        drop_client();
    }

    if (st.error == LLM_OK) {
        bool safety = strcmp(st.finish, "SAFETY") == 0 || strcmp(st.finish, "PROHIBITED_CONTENT") == 0 ||
                      strcmp(st.finish, "BLOCKLIST") == 0 || strcmp(st.finish, "SPII") == 0;
        if ((st.blocked || safety) && st.text.len == 0) {
            ESP_LOGW(TAG, "Request declined (%s)", st.blocked ? "prompt blocked" : st.finish);
            st.error = LLM_ERR_REFUSAL;
        } else if (strcmp(st.finish, "MAX_TOKENS") == 0) {
            ESP_LOGW(TAG, "Answer truncated (MAX_TOKENS)");
        }
        if (st.error == LLM_OK && st.text.len == 0) {
            ESP_LOGE(TAG, "No text in answer (finish: %s)", st.finish[0] ? st.finish : "-");
            st.error = LLM_ERR_OTHER;
        }
    }

    int64_t now = esp_timer_get_time();
    ESP_LOGI(TAG, "first text after %lld ms, total %lld ms, finish: %s, tokens in %d (cached %d), out %d "
             "(thoughts %d)", st.first_text_us ? (st.first_text_us - st.start_us) / 1000 : -1LL,
             (now - st.start_us) / 1000, st.finish[0] ? st.finish : "-", st.input_tokens, st.cached_tokens,
             st.output_tokens, st.thoughts_tokens);
    llm_last_stats = (llm_stats_t) {
        st.first_text_us ? (int)((st.first_text_us - st.start_us) / 1000) : -1, (int)((now - st.start_us) / 1000),
        st.output_tokens, st.thoughts_tokens
    };
    // promptTokenCount counts the cached ones; the thoughts are billed as output
    costs_add_tokens(COST_LLM, s_model, st.input_tokens - st.cached_tokens, st.cached_tokens, 0,
                     st.output_tokens + st.thoughts_tokens);

end:
    if (st.error == LLM_OK) {
        add_message("model", st.text.p, st.signature);
        trim_history();
        if (full_text) {
            *full_text = st.text.p;
            st.text.p = NULL;
        }
    } else {
        /* Drop the unanswered question so that the history keeps alternating user/model */
        cJSON_DeleteItemFromArray(s_messages, cJSON_GetArraySize(s_messages) - 1);
    }
    llm_sse_free(&st.sse);
    llm_buf_free(&st.text);
    free(st.signature);
    free(body);
    return st.error;
}
