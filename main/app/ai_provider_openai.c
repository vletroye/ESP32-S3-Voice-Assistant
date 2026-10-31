/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * GPT client: OpenAI Responses API with server-sent events (stream: true), stateless (store: false).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "cJSON.h"
#include "ai_provider_openai.h"
#include "file_costs.h"
#include "llm_common.h"

/* Counts the reasoning tokens too: room for a short spoken answer after a little reasoning */
#define OPENAI_MAX_OUTPUT_TOKENS    2048
#define OPENAI_URL_SIZE             160

static const char *TAG = "openai_chat";

static char *s_base_url = NULL;
static char s_url[OPENAI_URL_SIZE];
static char s_prewarm_url[OPENAI_URL_SIZE];
static char *s_auth = NULL;             /* "Bearer <key>" */
static char *s_model = NULL;
static char *s_instructions = NULL;
static char *s_effort = NULL;
/* Conversation history, sent with each request (nothing is stored on the OpenAI side) */
static cJSON *s_messages = NULL;
/* Kept between requests so that the TLS connection can be reused (keep-alive) */
static llm_http_t s_http = { .url = s_url, .key_header = "Authorization" };

typedef struct {
    llm_answer_t answer;
    llm_sse_t sse;
    char status[24];            /* "completed", "incomplete", "failed" */
    bool refused;
    bool done;                  /* response.completed / incomplete / failed received */
    llm_error_t error;

    int input_tokens;
    int cached_tokens;
    int output_tokens;
    int reasoning_tokens;
} stream_t;

void openai_chat_set_base_url(const char *base_url)
{
    llm_str_set(&s_base_url, base_url);
}

static void openai_reset(void *state)
{
    llm_history_reset(&s_messages);
}

static esp_err_t openai_init(void *state, const char *api_key, const char *model, const char *instructions,
                             const char *effort)
{
    const char *base_url = s_base_url;
    ESP_RETURN_ON_FALSE(base_url && api_key && api_key[0] && model && model[0], ESP_ERR_INVALID_ARG, TAG,
                        "URL, API key and model required");
    const char *sep = base_url[strlen(base_url) - 1] == '/' ? "" : "/";
    snprintf(s_url, sizeof(s_url), "%s%sresponses", base_url, sep);
    llm_str_set(&s_model, model);
    snprintf(s_prewarm_url, sizeof(s_prewarm_url), "%s%smodels/%s", base_url, sep, s_model);
    ESP_RETURN_ON_FALSE(llm_bearer_set(&s_auth, api_key), ESP_ERR_NO_MEM, TAG, "no memory");
    s_http.tag = TAG;
    s_http.key = s_auth;
    llm_str_set(&s_instructions, instructions);
    llm_str_set(&s_effort, effort);
    openai_reset(state);
    ESP_LOGI(TAG, "model: %s, reasoning effort: %s, streaming", s_model, s_effort ? s_effort : "(default)");
    return ESP_OK;
}

static void openai_set_instructions(void *state, const char *instructions)
{
    llm_str_set(&s_instructions, instructions);
}

/* Map an API error to the error kinds shown to the user */
static llm_error_t classify_error(int status, const char *code, const char *message)
{
    // First: "You exceeded your current quota, please check your plan and billing details" (429)
    if ((code && strcmp(code, "insufficient_quota") == 0) ||
            (message && (strcasestr(message, "quota") || strcasestr(message, "billing")))) {
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

/* {"error": {"code", "message"}} (HTTP errors, "response.failed") or {"code", "message"} ("error" event) */
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
    cJSON_AddNumberToObject(req, "max_output_tokens", OPENAI_MAX_OUTPUT_TOKENS);
    if (s_effort) {
        cJSON *reasoning = cJSON_AddObjectToObject(req, "reasoning");
        cJSON_AddStringToObject(reasoning, "effort", s_effort);
    }
    if (s_instructions) {
        cJSON_AddStringToObject(req, "instructions", s_instructions);
    }
    /* Referenced, not copied: the history stays owned by s_messages */
    cJSON_AddItemReferenceToObject(req, "input", s_messages);
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
    if ((v = cJSON_GetObjectItem(cJSON_GetObjectItem(usage, "input_tokens_details"), "cached_tokens")) &&
            cJSON_IsNumber(v)) {
        st->cached_tokens = v->valueint;
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
            llm_answer_add(&st->answer, delta->valuestring, strlen(delta->valuestring));
        }
    } else if (strcmp(t, "response.refusal.delta") == 0) {
        st->refused = true;
    } else if (strcmp(t, "response.completed") == 0 || strcmp(t, "response.incomplete") == 0) {
        on_response_end(st, cJSON_GetObjectItem(json, "response"));
    } else if (strcmp(t, "response.failed") == 0) {
        const cJSON *response = cJSON_GetObjectItem(json, "response");
        on_response_end(st, response);
        st->error = error_from_json(NULL, 200, response);
    } else if (strcmp(t, "error") == 0) {
        st->error = error_from_json(NULL, 200, json);
        st->done = true;
    }
    /* Other events (created, in_progress, output_item, reasoning...) are ignored */
    cJSON_Delete(json);
}

/* ---------- HTTP ---------- */

static void openai_prewarm(void *state)
{
    // A free request with a tiny answer (the model's description): it opens the TLS connection
    llm_http_prewarm(&s_http, s_prewarm_url, NULL, 0);
}

static const cJSON *openai_history(void *state)
{
    return s_messages;
}

static void openai_set_history(void *state, cJSON *messages)
{
    llm_history_set(&s_messages, messages);
}

static llm_error_t openai_stream(void *state, const char *text, llm_text_cb_t on_text, void *ctx, char **full_text)
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

    ESP_LOGI(TAG, "first text after %d ms, total %d ms, status: %s, tokens in %d (cached %d), out %d (reasoning %d)",
             llm_answer_first_ms(&st.answer), llm_answer_total_ms(&st.answer), st.status[0] ? st.status : "-",
             st.input_tokens, st.cached_tokens, st.output_tokens, st.reasoning_tokens);
    // output_tokens counts the reasoning too
    llm_answer_stats(&st.answer, st.output_tokens - st.reasoning_tokens, st.reasoning_tokens);
    // input_tokens counts the cached ones; output_tokens the reasoning (billed too)
    costs_add_tokens(COST_LLM, s_model, st.input_tokens - st.cached_tokens, st.cached_tokens, 0, st.output_tokens);

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

const llm_backend_t llm_backend_openai = {
    .key_name = "OpenAI_key",
    .init = openai_init,
    .set_instructions = openai_set_instructions,
    .reset = openai_reset,
    .history = openai_history,
    .set_history = openai_set_history,
    .prewarm = openai_prewarm,
    .stream = openai_stream,
};
