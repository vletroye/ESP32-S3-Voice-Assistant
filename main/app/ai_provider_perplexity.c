/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Perplexity client (Sonar): its Agent API (/v1/agent, shaped like OpenAI's Responses API, which replaced the
 * Sonar Chat Completions in September 2026) with server-sent events, stateless (store: false), the conversation
 * kept on the device. Its web search is offered with every question; the "[1]" marks of its sources are taken out
 * of the text (spoken and shown).
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
#include "ai_provider_perplexity.h"
#include "file_costs.h"
#include "llm_sse.h"

#define PERPLEXITY_URL                  "https://api.perplexity.ai/v1/agent"
/* A tiny free answer (the list of models): opens the TLS connection in advance */
#define PERPLEXITY_PREWARM_URL          "https://api.perplexity.ai/v1/models"
/* Counts the reasoning tokens too: room for a short spoken answer after a little reasoning */
#define PERPLEXITY_MAX_OUTPUT_TOKENS    2048
/* Web pages read for an answer: enough for a short spoken one (each one costs input tokens) */
#define PERPLEXITY_SEARCH_RESULTS       5
/* Longest source mark taken out of the text: "[12, 13]" */
#define PERPLEXITY_MARK_MAX             12
#define PERPLEXITY_TIMEOUT_MS           30000
#define PERPLEXITY_READ_CHUNK           512
/* History kept for follow-up questions: the last 10 exchanges (user + assistant messages) */
#define PERPLEXITY_HISTORY_MAX_MSGS     24
/* Past the maximum, cut down to this size in one go, so that the start (and the prompt cache) stays */
#define PERPLEXITY_HISTORY_TRIM_TO      12

static const char *TAG = "perplexity";

static char *s_auth = NULL;             /* "Bearer <key>" */
static char *s_model = NULL;
static char *s_instructions = NULL;
static char *s_effort = NULL;
/* Conversation history: [{role, content}], sent with each request (nothing is stored on Perplexity's side) */
static cJSON *s_messages = NULL;
/* Kept between requests so that the TLS connection can be reused (keep-alive) */
static esp_http_client_handle_t s_client = NULL;

typedef struct {
    llm_text_cb_t on_text;
    void *ctx;
    llm_sse_t sse;
    llm_buf_t text;             /* whole answer text */
    char status[24];            /* "completed", "incomplete", "failed" */
    bool refused;
    bool done;                  /* response.completed / incomplete / failed received */
    llm_error_t error;
    char mark[PERPLEXITY_MARK_MAX + 1];  /* "[1" held back: a source mark, or text if it does not end so */
    size_t mark_len;
    int searches;               /* web searches made (billed per call) */

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

esp_err_t perplexity_chat_init(const char *api_key, const char *model, const char *instructions, const char *effort)
{
    ESP_RETURN_ON_FALSE(api_key && api_key[0] && model && model[0], ESP_ERR_INVALID_ARG, TAG,
                        "API key and model required");
    replace_str(&s_model, model);
    free(s_auth);
    s_auth = malloc(strlen(api_key) + 8);
    ESP_RETURN_ON_FALSE(s_auth, ESP_ERR_NO_MEM, TAG, "no memory");
    sprintf(s_auth, "Bearer %s", api_key);
    replace_str(&s_instructions, instructions);
    replace_str(&s_effort, (effort && strcmp(effort, "-") != 0) ? effort : NULL);
    perplexity_chat_reset();
    ESP_LOGI(TAG, "model: %s, reasoning effort: %s, streaming", s_model, s_effort ? s_effort : "(default)");
    return ESP_OK;
}

void perplexity_chat_set_instructions(const char *instructions)
{
    replace_str(&s_instructions, instructions);
}

void perplexity_chat_reset(void)
{
    cJSON_Delete(s_messages);
    s_messages = cJSON_CreateArray();
}

/* Map an API error to the error kinds shown to the user */
static llm_error_t classify_error(int status, const char *code, const char *message)
{
    // First: a valid key without paid credit gets a 403 ("restricted_api_key: The Agent API requires paid credits")
    if (status == 402 || (code && strstr(code, "quota")) ||
            (message && (strcasestr(message, "credit") || strcasestr(message, "quota") ||
                         strcasestr(message, "balance")))) {
        return LLM_ERR_CREDIT;
    }
    if (status == 401 || status == 403 || (code && strcmp(code, "invalid_api_key") == 0)) {
        return LLM_ERR_AUTH;
    }
    if (status == 429 || status >= 500 || (code && (strcmp(code, "rate_limit_exceeded") == 0 ||
                                                     strcmp(code, "server_error") == 0))) {
        return LLM_ERR_BUSY;
    }
    return LLM_ERR_OTHER;
}

/*
 * {"error": {"message", "type", "code": 401}} (HTTP errors, "response.failed") or {"code", "message"} ("error"
 * event); the code is the HTTP status, or a name
 */
static llm_error_t error_from_json(int status, const cJSON *json)
{
    const cJSON *api_error = cJSON_GetObjectItem(json, "error");
    if (!cJSON_IsObject(api_error)) {
        api_error = json;
    }
    const cJSON *code = cJSON_GetObjectItem(api_error, "code");
    if (cJSON_IsNumber(code)) {
        status = code->valueint;
    }
    if (!cJSON_IsString(code)) {
        code = cJSON_GetObjectItem(api_error, "type");
    }
    const cJSON *message = cJSON_GetObjectItem(api_error, "message");
    const char *code_str = cJSON_IsString(code) ? code->valuestring : NULL;
    const char *message_str = cJSON_IsString(message) ? message->valuestring : NULL;
    ESP_LOGE(TAG, "HTTP %d: %s: %s", status, code_str ? code_str : "?", message_str ? message_str : "?");
    return classify_error(status, code_str, message_str);
}

static char *build_request(void)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", s_model);
    cJSON_AddBoolToObject(req, "stream", true);
    cJSON_AddBoolToObject(req, "store", false);
    cJSON_AddNumberToObject(req, "max_output_tokens", PERPLEXITY_MAX_OUTPUT_TOKENS);
    // Its web search: what Perplexity is for (the model decides whether a question needs it)
    cJSON *search = cJSON_CreateObject();
    cJSON_AddStringToObject(search, "type", "web_search");
    cJSON_AddStringToObject(search, "search_type", "web");     // the full search ("fast": quicker, less thorough)
    cJSON_AddNumberToObject(search, "max_results", PERPLEXITY_SEARCH_RESULTS);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(req, "tools"), search);
    if (s_effort) {
        cJSON *reasoning = cJSON_AddObjectToObject(req, "reasoning");
        cJSON_AddStringToObject(reasoning, "effort", s_effort);
    }
    if (s_instructions) {
        cJSON_AddStringToObject(req, "instructions", s_instructions);
    }
    /* The history as input items: each message with its type (the history itself stays as other clients have it) */
    cJSON *input = cJSON_AddArrayToObject(req, "input");
    const cJSON *msg;
    cJSON_ArrayForEach(msg, s_messages) {
        cJSON *item = cJSON_Duplicate(msg, true);
        cJSON_AddStringToObject(item, "type", "message");
        cJSON_AddItemToArray(input, item);
    }
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return body;
}

/* ---------- SSE event handling ---------- */

static void on_response_end(stream_t *st, const cJSON *response)
{
    const cJSON *status = cJSON_GetObjectItem(response, "status");
    if (cJSON_IsString(status)) {
        strlcpy(st->status, status->valuestring, sizeof(st->status));
    }
    const cJSON *usage = cJSON_GetObjectItem(response, "usage");
    const cJSON *v;
    if ((v = cJSON_GetObjectItem(usage, "input_tokens")) && cJSON_IsNumber(v)) {
        st->input_tokens = v->valueint;
    }
    if ((v = cJSON_GetObjectItem(cJSON_GetObjectItem(usage, "input_tokens_details"), "cache_read_input_tokens")) &&
            cJSON_IsNumber(v)) {
        st->cached_tokens = v->valueint;
    }
    // {"search_web": {"invocation": 1, "cost_usd": ...}}: the searches, billed per call
    const cJSON *tool;
    cJSON_ArrayForEach(tool, cJSON_GetObjectItem(usage, "tool_calls_details")) {
        if ((v = cJSON_GetObjectItem(tool, "invocation")) && cJSON_IsNumber(v)) {
            st->searches += v->valueint;
        }
    }
    if ((v = cJSON_GetObjectItem(usage, "output_tokens")) && cJSON_IsNumber(v)) {
        st->output_tokens = v->valueint;
    }
    if ((v = cJSON_GetObjectItem(cJSON_GetObjectItem(usage, "output_tokens_details"), "reasoning_tokens")) &&
            cJSON_IsNumber(v)) {
        st->reasoning_tokens = v->valueint;
    }
    st->done = true;
}

/* Text given to the answer (shown, spoken and kept) */
static void emit_text(stream_t *st, const char *text, size_t len)
{
    if (len == 0) {
        return;
    }
    llm_buf_append(&st->text, text, len);
    if (!st->first_text_us) {
        st->first_text_us = esp_timer_get_time();
    }
    if (st->on_text) {
        char *piece = strndup(text, len);
        if (piece) {
            st->on_text(piece, st->ctx);
            free(piece);
        }
    }
}

/* The source marks ("[1]", "[2, 3]") taken out, even when a delta cuts them; "[" followed by other text kept */
static void add_text(stream_t *st, const char *delta)
{
    const char *start = delta;
    for (const char *p = delta; *p; p++) {
        if (st->mark_len == 0) {
            if (*p == '[') {
                emit_text(st, start, p - start);
                st->mark[st->mark_len++] = '[';
                start = p + 1;
            }
            continue;
        }
        bool in_mark = (*p >= '0' && *p <= '9') || ((*p == ',' || *p == ' ') && st->mark_len > 1);
        if (*p == ']' && st->mark_len > 1) {
            st->mark_len = 0;           // a whole mark: dropped
        } else if (in_mark && st->mark_len < PERPLEXITY_MARK_MAX) {
            st->mark[st->mark_len++] = *p;
        } else {
            emit_text(st, st->mark, st->mark_len);      // not a mark: given back, as the text after it
            st->mark_len = 0;
            start = p;
            p--;                        // this character read again, out of a mark
            continue;
        }
        start = p + 1;
    }
    if (st->mark_len == 0) {
        emit_text(st, start, strlen(start));
    }
}

static void sse_dispatch(const char *data, void *ctx)
{
    stream_t *st = (stream_t *)ctx;
    cJSON *json = cJSON_Parse(data);
    if (!json) {
        ESP_LOGW(TAG, "Unparsable event: %.120s", data);
        return;
    }
    const cJSON *type = cJSON_GetObjectItem(json, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";

    if (strcmp(t, "response.output_text.delta") == 0) {
        const cJSON *delta = cJSON_GetObjectItem(json, "delta");
        if (cJSON_IsString(delta)) {
            add_text(st, delta->valuestring);
        }
    } else if (strcmp(t, "response.refusal.delta") == 0) {
        st->refused = true;
    } else if (strcmp(t, "response.completed") == 0 || strcmp(t, "response.incomplete") == 0) {
        emit_text(st, st->mark, st->mark_len);         // an opened "[" at the very end: text
        st->mark_len = 0;
        on_response_end(st, cJSON_GetObjectItem(json, "response"));
    } else if (strcmp(t, "response.failed") == 0) {
        const cJSON *response = cJSON_GetObjectItem(json, "response");
        on_response_end(st, response);
        st->error = error_from_json(200, response);
    } else if (strcmp(t, "error") == 0) {
        st->error = error_from_json(200, json);
        st->done = true;
    }
    /* Other events (created, in_progress, output_item, reasoning, search queries and results...) are ignored */
    cJSON_Delete(json);
}

/* ---------- HTTP ---------- */

static esp_http_client_handle_t get_client(void)
{
    if (!s_client) {
        esp_http_client_config_t config = {
            .url = PERPLEXITY_URL,
            .method = HTTP_METHOD_POST,
            .timeout_ms = PERPLEXITY_TIMEOUT_MS,
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

void perplexity_chat_prewarm(void)
{
    if (!s_auth) {
        return;
    }
    int64_t t0 = esp_timer_get_time();
    esp_http_client_handle_t client = get_client();
    if (!client) {
        return;
    }
    /* A free request with a small answer (the models): it opens the TLS connection */
    esp_http_client_set_url(client, PERPLEXITY_PREWARM_URL);
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
        esp_http_client_set_url(client, PERPLEXITY_URL);
        esp_http_client_set_method(client, HTTP_METHOD_POST);
    } else {
        drop_client();
    }
    ESP_LOGI(TAG, "prewarm: HTTP %d in %lld ms", status, (esp_timer_get_time() - t0) / 1000);
}

/* Keep the history short; older exchanges are dropped in user/assistant pairs */
static void trim_history(void)
{
    if (cJSON_GetArraySize(s_messages) <= PERPLEXITY_HISTORY_MAX_MSGS) {
        return;
    }
    while (cJSON_GetArraySize(s_messages) > PERPLEXITY_HISTORY_TRIM_TO) {
        cJSON_DeleteItemFromArray(s_messages, 0);
        cJSON_DeleteItemFromArray(s_messages, 0);
    }
}

const cJSON *perplexity_chat_history(void)
{
    return s_messages;
}

void perplexity_chat_set_history(cJSON *messages)
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

llm_error_t perplexity_chat_stream(const char *text, llm_text_cb_t on_text, void *ctx, char **full_text)
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

    char chunk[PERPLEXITY_READ_CHUNK];
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

    while (!st.done && !llm_cancelled() && (n = esp_http_client_read(s_client, chunk, sizeof(chunk))) > 0) {
        llm_sse_feed(&st.sse, chunk, n);
    }
    if (st.error == LLM_OK && !st.done) {
        ESP_LOGE(TAG, "Stream ended before response.completed");
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
        if (st.refused && st.text.len == 0) {
            ESP_LOGW(TAG, "Request declined (refusal)");
            st.error = LLM_ERR_REFUSAL;
        } else if (strcmp(st.status, "incomplete") == 0) {
            ESP_LOGW(TAG, "Answer truncated (incomplete)");
        }
        if (st.error == LLM_OK && st.text.len == 0) {
            ESP_LOGE(TAG, "No text in answer (status: %s)", st.status);
            st.error = LLM_ERR_OTHER;
        }
    }

    int64_t now = esp_timer_get_time();
    ESP_LOGI(TAG, "first text after %lld ms, total %lld ms, status: %s, tokens in %d (cached %d), out %d "
             "(reasoning %d), %d web search(es)", st.first_text_us ? (st.first_text_us - st.start_us) / 1000 : -1LL,
             (now - st.start_us) / 1000, st.status[0] ? st.status : "-", st.input_tokens, st.cached_tokens,
             st.output_tokens, st.reasoning_tokens, st.searches);
    // output_tokens counts the reasoning too
    llm_last_stats = (llm_stats_t) {
        st.first_text_us ? (int)((st.first_text_us - st.start_us) / 1000) : -1, (int)((now - st.start_us) / 1000),
        st.output_tokens - st.reasoning_tokens, st.reasoning_tokens
    };
    // input_tokens counts the cached ones; output_tokens the reasoning (billed too)
    costs_add_tokens(COST_LLM, s_model, st.input_tokens - st.cached_tokens, st.cached_tokens, 0, st.output_tokens);
    costs_add_calls("perplexity:web_search", st.searches);

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
