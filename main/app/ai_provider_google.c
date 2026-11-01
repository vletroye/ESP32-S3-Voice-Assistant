/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Gemini client: Gemini API generateContent with server-sent events (streamGenerateContent?alt=sse),
 * the conversation kept on the device and sent with each request. The effort of AGENTS.INI is its
 * thinkingConfig.thinkingLevel ("minimal", "low", ...).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "cJSON.h"
#include "ai_provider_google.h"
#include "file_costs.h"
#include "llm_common.h"

#define GEMINI_BASE_URL             "https://generativelanguage.googleapis.com/v1beta/models/"
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
static llm_http_t s_http = { .url = s_url, .key_header = "x-goog-api-key" };

typedef struct {
    llm_answer_t answer;
    llm_sse_t sse;
    char *signature;            /* last thought signature, sent back with the answer */
    char finish[32];            /* finishReason: "STOP", "MAX_TOKENS", "SAFETY"... */
    bool blocked;               /* prompt blocked (promptFeedback.blockReason) */
    bool done;                  /* finishReason received */
    llm_error_t error;

    int input_tokens;
    int cached_tokens;
    int output_tokens;
    int thoughts_tokens;
} stream_t;

static void gemini_reset(void *state)
{
    llm_history_reset(&s_messages);
}

static esp_err_t gemini_init(void *state, const char *api_key, const char *model, const char *instructions,
                             const char *thinking_level)
{
    ESP_RETURN_ON_FALSE(api_key && api_key[0] && model && model[0], ESP_ERR_INVALID_ARG, TAG,
                        "API key and model required");
    llm_str_set(&s_model, model);
    snprintf(s_url, sizeof(s_url), GEMINI_BASE_URL "%s:streamGenerateContent?alt=sse", s_model);
    snprintf(s_prewarm_url, sizeof(s_prewarm_url), GEMINI_BASE_URL "%s", s_model);
    llm_str_set(&s_key, api_key);
    s_http.tag = TAG;
    s_http.key = s_key;
    llm_str_set(&s_instructions, instructions);
    llm_str_set(&s_thinking_level, thinking_level);
    gemini_reset(state);
    ESP_LOGI(TAG, "model: %s, thinking level: %s, streaming", s_model,
             s_thinking_level ? s_thinking_level : "(default)");
    return ESP_OK;
}

static void gemini_set_instructions(void *state, const char *instructions)
{
    llm_str_set(&s_instructions, instructions);
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
static llm_error_t error_from_json(const void *ctx, int status, const cJSON *json)
{
    if (!json) {
        return classify_error(status, NULL, NULL);
    }
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
    cJSON_AddNumberToObject(config, "maxOutputTokens", llm_max_output_tokens());
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
        st->error = error_from_json(NULL, 200, json);
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
            llm_str_set(&st->signature, signature->valuestring);
        }
        const cJSON *text = cJSON_GetObjectItem(part, "text");
        if (cJSON_IsTrue(cJSON_GetObjectItem(part, "thought")) || !cJSON_IsString(text)) {
            continue;   // thought summaries are not spoken
        }
        llm_answer_add(&st->answer, text->valuestring, strlen(text->valuestring));
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

static void gemini_prewarm(void *state)
{
    // A free request with a tiny answer (the model's description): it opens the TLS connection
    llm_http_prewarm(&s_http, s_prewarm_url, NULL, 0);
}

static const cJSON *gemini_history(void *state)
{
    return s_messages;
}

static void gemini_set_history(void *state, cJSON *messages)
{
    llm_history_set(&s_messages, messages);
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

static llm_error_t gemini_stream(void *state, const char *text, llm_text_cb_t on_text, void *ctx, char **full_text)
{
    if (full_text) {
        *full_text = NULL;
    }
    if (!s_key || !s_messages) {
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
    add_message("user", text, NULL);

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

    /* The last event carries finishReason; the body ends right after it */
    llm_http_stream(&s_http, &st.sse, NULL);
    if (st.error == LLM_OK && !st.done) {
        ESP_LOGE(TAG, "Stream ended before finishReason");
        st.error = LLM_ERR_NETWORK;
    }
    llm_http_finish(&s_http, st.error == LLM_OK);

    if (st.error == LLM_OK) {
        bool safety = strcmp(st.finish, "SAFETY") == 0 || strcmp(st.finish, "PROHIBITED_CONTENT") == 0 ||
                      strcmp(st.finish, "BLOCKLIST") == 0 || strcmp(st.finish, "SPII") == 0;
        if ((st.blocked || safety) && st.answer.text.len == 0) {
            ESP_LOGW(TAG, "Request declined (%s)", st.blocked ? "prompt blocked" : st.finish);
            st.error = LLM_ERR_REFUSAL;
        } else if (strcmp(st.finish, "MAX_TOKENS") == 0) {
            ESP_LOGW(TAG, "Answer truncated (MAX_TOKENS)");
        }
        if (st.error == LLM_OK && st.answer.text.len == 0) {
            ESP_LOGE(TAG, "No text in answer (finish: %s)", st.finish[0] ? st.finish : "-");
            st.error = LLM_ERR_OTHER;
        }
    }

    ESP_LOGI(TAG, "first text after %d ms, total %d ms, finish: %s, tokens in %d (cached %d), out %d (thoughts %d)",
             llm_answer_first_ms(&st.answer), llm_answer_total_ms(&st.answer), st.finish[0] ? st.finish : "-",
             st.input_tokens, st.cached_tokens, st.output_tokens, st.thoughts_tokens);
    llm_answer_stats(&st.answer, st.output_tokens, st.thoughts_tokens);
    // promptTokenCount counts the cached ones; the thoughts are billed as output
    costs_add_tokens(COST_LLM, s_model, st.input_tokens - st.cached_tokens, st.cached_tokens, 0,
                     st.output_tokens + st.thoughts_tokens);

end:
    if (st.error == LLM_OK) {
        add_message("model", st.answer.text.p, st.signature);
        llm_history_trim(s_messages, NULL);
        if (full_text) {
            *full_text = st.answer.text.p;
            st.answer.text.p = NULL;
        }
    } else {
        /* Drop the unanswered question so that the history keeps alternating user/model */
        llm_history_drop_from(s_messages, start_count);
    }
    llm_sse_free(&st.sse);
    llm_buf_free(&st.answer.text);
    free(st.signature);
    free(body);
    return st.error;
}

const llm_backend_t llm_backend_gemini = {
    .init = gemini_init,
    .set_instructions = gemini_set_instructions,
    .reset = gemini_reset,
    .history = gemini_history,
    .set_history = gemini_set_history,
    .prewarm = gemini_prewarm,
    .stream = gemini_stream,
};
