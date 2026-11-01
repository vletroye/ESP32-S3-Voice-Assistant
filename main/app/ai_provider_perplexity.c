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
#include "esp_check.h"
#include "cJSON.h"
#include "ai_provider_perplexity.h"
#include "file_costs.h"
#include "llm_common.h"

#define PERPLEXITY_URL                  "https://api.perplexity.ai/v1/agent"
/* A tiny free answer (the list of models): opens the TLS connection in advance */
#define PERPLEXITY_PREWARM_URL          "https://api.perplexity.ai/v1/models"
/* Web pages read for an answer: enough for a short spoken one (each one costs input tokens) */
#define PERPLEXITY_SEARCH_RESULTS       5
/* Longest source mark taken out of the text: "[12, 13]" */
#define PERPLEXITY_MARK_MAX             12

static const char *TAG = "perplexity";

static char *s_auth = NULL;             /* "Bearer <key>" */
static char *s_model = NULL;
static char *s_instructions = NULL;
static char *s_effort = NULL;
/* Conversation history: [{role, content}], sent with each request (nothing is stored on Perplexity's side) */
static cJSON *s_messages = NULL;
/* Kept between requests so that the TLS connection can be reused (keep-alive) */
static llm_http_t s_http = { .url = PERPLEXITY_URL, .key_header = "Authorization" };

typedef struct {
    llm_answer_t answer;
    llm_sse_t sse;
    char status[24];            /* "completed", "incomplete", "failed" */
    bool refused;
    bool done;                  /* response.completed / incomplete / failed received */
    llm_error_t error;
    char mark[PERPLEXITY_MARK_MAX + 1];  /* "[1" held back: a source mark, or text if it does not end so */
    size_t mark_len;
    int searches;               /* web searches made (billed per call) */

    int input_tokens;
    int cached_tokens;
    int output_tokens;
    int reasoning_tokens;
} stream_t;

static void perplexity_reset(void *state)
{
    llm_history_reset(&s_messages);
}

static esp_err_t perplexity_init(void *state, const char *api_key, const char *model, const char *instructions,
                                 const char *effort)
{
    ESP_RETURN_ON_FALSE(api_key && api_key[0] && model && model[0], ESP_ERR_INVALID_ARG, TAG,
                        "API key and model required");
    llm_str_set(&s_model, model);
    ESP_RETURN_ON_FALSE(llm_bearer_set(&s_auth, api_key), ESP_ERR_NO_MEM, TAG, "no memory");
    s_http.tag = TAG;
    s_http.key = s_auth;
    llm_str_set(&s_instructions, instructions);
    llm_str_set(&s_effort, (effort && strcmp(effort, "-") != 0) ? effort : NULL);
    perplexity_reset(state);
    ESP_LOGI(TAG, "model: %s, reasoning effort: %s, streaming", s_model, s_effort ? s_effort : "(default)");
    return ESP_OK;
}

static void perplexity_set_instructions(void *state, const char *instructions)
{
    llm_str_set(&s_instructions, instructions);
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
static llm_error_t error_from_json(const void *ctx, int status, const cJSON *json)
{
    if (!json) {
        return classify_error(status, NULL, NULL);
    }
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
    cJSON_AddNumberToObject(req, "max_output_tokens", llm_max_output_tokens());
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
    llm_answer_add(&st->answer, text, len);
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
        st->error = error_from_json(NULL, 200, response);
    } else if (strcmp(t, "error") == 0) {
        st->error = error_from_json(NULL, 200, json);
        st->done = true;
    }
    /* Other events (created, in_progress, output_item, reasoning, search queries and results...) are ignored */
    cJSON_Delete(json);
}

/* ---------- HTTP ---------- */

static void perplexity_prewarm(void *state)
{
    // A free request with a small answer (the models): it opens the TLS connection
    llm_http_prewarm(&s_http, PERPLEXITY_PREWARM_URL, NULL, 0);
}

static const cJSON *perplexity_history(void *state)
{
    return s_messages;
}

static void perplexity_set_history(void *state, cJSON *messages)
{
    llm_history_set(&s_messages, messages);
}

static llm_error_t perplexity_stream(void *state, const char *text, llm_text_cb_t on_text, void *ctx,
                                     char **full_text)
{
    if (full_text) {
        *full_text = NULL;
    }
    if (!s_auth || !s_messages) {
        return LLM_ERR_NO_KEY;
    }

    stream_t st = {
        .sse = { .on_event = sse_dispatch },
        .error = LLM_OK,
    };
    st.sse.ctx = &st;
    llm_answer_begin(&st.answer, on_text, ctx);
    int status = 0;
    char *body = NULL;

    /* Append the question; removed again if the exchange fails */
    int start_count = cJSON_GetArraySize(s_messages);
    llm_history_add(s_messages, "user", text);

    body = build_request();
    if (!body) {
        st.error = LLM_ERR_OTHER;
        goto end;
    }
    if (llm_http_send(&s_http, body, &status) != ESP_OK) {
        st.error = LLM_ERR_NETWORK;
        goto end;
    }
    if (status != 200) {
        st.error = llm_http_error(&s_http, status, error_from_json, NULL, NULL);
        goto end;
    }

    llm_http_stream(&s_http, &st.sse, &st.done);
    if (st.error == LLM_OK && !st.done) {
        ESP_LOGE(TAG, "Stream ended before response.completed");
        st.error = LLM_ERR_NETWORK;
    }
    llm_http_finish(&s_http, st.error == LLM_OK);

    if (st.error == LLM_OK) {
        if (st.refused && st.answer.text.len == 0) {
            ESP_LOGW(TAG, "Request declined (refusal)");
            st.error = LLM_ERR_REFUSAL;
        } else if (strcmp(st.status, "incomplete") == 0) {
            ESP_LOGW(TAG, "Answer truncated (incomplete)");
        }
        if (st.error == LLM_OK && st.answer.text.len == 0) {
            ESP_LOGE(TAG, "No text in answer (status: %s)", st.status);
            st.error = LLM_ERR_OTHER;
        }
    }

    ESP_LOGI(TAG, "first text after %d ms, total %d ms, status: %s, tokens in %d (cached %d), out %d "
             "(reasoning %d), %d web search(es)", llm_answer_first_ms(&st.answer), llm_answer_total_ms(&st.answer),
             st.status[0] ? st.status : "-", st.input_tokens, st.cached_tokens, st.output_tokens,
             st.reasoning_tokens, st.searches);
    // output_tokens counts the reasoning too
    llm_answer_stats(&st.answer, st.output_tokens - st.reasoning_tokens, st.reasoning_tokens);
    // input_tokens counts the cached ones; output_tokens the reasoning (billed too)
    costs_add_tokens(COST_LLM, s_model, st.input_tokens - st.cached_tokens, st.cached_tokens, 0, st.output_tokens);
    costs_add_calls("perplexity:web_search", st.searches);

end:
    if (st.error == LLM_OK) {
        llm_history_add(s_messages, "assistant", st.answer.text.p);
        llm_history_trim(s_messages, NULL);
        if (full_text) {
            *full_text = st.answer.text.p;
            st.answer.text.p = NULL;
        }
    } else {
        /* Drop the unanswered question so that the history keeps alternating user/assistant */
        llm_history_drop_from(s_messages, start_count);
    }
    llm_sse_free(&st.sse);
    llm_buf_free(&st.answer.text);
    free(body);
    return st.error;
}

const llm_backend_t llm_backend_perplexity = {
    .init = perplexity_init,
    .set_instructions = perplexity_set_instructions,
    .reset = perplexity_reset,
    .history = perplexity_history,
    .set_history = perplexity_set_history,
    .prewarm = perplexity_prewarm,
    .stream = perplexity_stream,
};
