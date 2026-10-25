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
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "openai_chat.h"
#include "llm_sse.h"

/* Counts the reasoning tokens too: room for a short spoken answer after a little reasoning */
#define OPENAI_MAX_OUTPUT_TOKENS    2048
#define OPENAI_TIMEOUT_MS           30000
#define OPENAI_READ_CHUNK           512
/* History kept for follow-up questions: the last 10 exchanges (user + assistant messages) */
#define OPENAI_HISTORY_MAX_MSGS     20
#define OPENAI_URL_SIZE             160

static const char *TAG = "openai_chat";

static char s_url[OPENAI_URL_SIZE];
static char s_prewarm_url[OPENAI_URL_SIZE];
static char *s_auth = NULL;             /* "Bearer <key>" */
static char *s_model = NULL;
static char *s_instructions = NULL;
static char *s_effort = NULL;
/* Conversation history, sent with each request (nothing is stored on the OpenAI side) */
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

esp_err_t openai_chat_init(const char *base_url, const char *api_key, const char *model, const char *instructions,
                           const char *effort)
{
    ESP_RETURN_ON_FALSE(base_url && api_key && api_key[0] && model && model[0], ESP_ERR_INVALID_ARG, TAG,
                        "URL, API key and model required");
    const char *sep = (base_url[0] && base_url[strlen(base_url) - 1] == '/') ? "" : "/";
    snprintf(s_url, sizeof(s_url), "%s%sresponses", base_url, sep);
    replace_str(&s_model, model);
    snprintf(s_prewarm_url, sizeof(s_prewarm_url), "%s%smodels/%s", base_url, sep, s_model);
    free(s_auth);
    s_auth = malloc(strlen(api_key) + 8);
    ESP_RETURN_ON_FALSE(s_auth, ESP_ERR_NO_MEM, TAG, "no memory");
    sprintf(s_auth, "Bearer %s", api_key);
    replace_str(&s_instructions, instructions);
    replace_str(&s_effort, effort);
    openai_chat_reset();
    ESP_LOGI(TAG, "model: %s, reasoning effort: %s, streaming", s_model, s_effort ? s_effort : "(default)");
    return ESP_OK;
}

void openai_chat_reset(void)
{
    cJSON_Delete(s_messages);
    s_messages = cJSON_CreateArray();
}

/* Map an API error to the error kinds shown to the user */
static llm_error_t classify_error(int status, const char *code, const char *message)
{
    if (status == 401 || status == 403 || (code && strcmp(code, "invalid_api_key") == 0)) {
        return LLM_ERR_AUTH;
    }
    if ((code && strcmp(code, "insufficient_quota") == 0) || (message && strstr(message, "quota"))) {
        return LLM_ERR_CREDIT;
    }
    if (status == 429 || status >= 500 || (code && (strcmp(code, "rate_limit_exceeded") == 0 ||
                                                     strcmp(code, "server_error") == 0))) {
        return LLM_ERR_BUSY;
    }
    return LLM_ERR_OTHER;
}

/* {"error": {"code", "message"}} (HTTP errors, "response.failed") or {"code", "message"} ("error" event) */
static llm_error_t error_from_json(int status, const cJSON *json)
{
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
        if (cJSON_IsString(delta) && delta->valuestring[0]) {
            llm_buf_append(&st->text, delta->valuestring, strlen(delta->valuestring));
            if (!st->first_text_us) {
                st->first_text_us = esp_timer_get_time();
            }
            if (st->on_text) {
                st->on_text(delta->valuestring, st->ctx);
            }
        }
    } else if (strcmp(t, "response.refusal.delta") == 0) {
        st->refused = true;
    } else if (strcmp(t, "response.completed") == 0 || strcmp(t, "response.incomplete") == 0) {
        on_response_end(st, cJSON_GetObjectItem(json, "response"));
    } else if (strcmp(t, "response.failed") == 0) {
        const cJSON *response = cJSON_GetObjectItem(json, "response");
        on_response_end(st, response);
        st->error = error_from_json(200, response);
    } else if (strcmp(t, "error") == 0) {
        st->error = error_from_json(200, json);
        st->done = true;
    }
    /* Other events (created, in_progress, output_item, reasoning...) are ignored */
    cJSON_Delete(json);
}

/* ---------- HTTP ---------- */

static esp_http_client_handle_t get_client(void)
{
    if (!s_client) {
        esp_http_client_config_t config = {
            .url = s_url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = OPENAI_TIMEOUT_MS,
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

void openai_chat_prewarm(void)
{
    if (!s_auth) {
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

/* Keep the history short; older exchanges are dropped in user/assistant pairs */
static void trim_history(void)
{
    while (cJSON_GetArraySize(s_messages) > OPENAI_HISTORY_MAX_MSGS) {
        cJSON_DeleteItemFromArray(s_messages, 0);
        cJSON_DeleteItemFromArray(s_messages, 0);
    }
}

const cJSON *openai_chat_history(void)
{
    return s_messages;
}

void openai_chat_set_history(cJSON *messages)
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

llm_error_t openai_chat_stream(const char *text, llm_text_cb_t on_text, void *ctx, char **full_text)
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

    char chunk[OPENAI_READ_CHUNK];
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

    while (!st.done && (n = esp_http_client_read(s_client, chunk, sizeof(chunk))) > 0) {
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
             "(reasoning %d)", st.first_text_us ? (st.first_text_us - st.start_us) / 1000 : -1LL,
             (now - st.start_us) / 1000, st.status[0] ? st.status : "-", st.input_tokens, st.cached_tokens,
             st.output_tokens, st.reasoning_tokens);

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
