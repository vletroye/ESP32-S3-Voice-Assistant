/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Claude client: Anthropic Messages API with server-sent events (stream: true).
 */

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "claude_client.h"
#include "llm_sse.h"

#define CLAUDE_URL              "https://api.anthropic.com/v1/messages"
#define CLAUDE_PREWARM_URL      "https://api.anthropic.com/v1/models?limit=1"
#define CLAUDE_API_VERSION      "2023-06-01"
/* Spoken answers are short; the ceiling leaves room for Opus' thinking, which cannot be disabled */
#define CLAUDE_MAX_TOKENS       2048
#define CLAUDE_TIMEOUT_MS       30000
#define CLAUDE_READ_CHUNK       512
/* History kept for follow-up questions: the last 10 exchanges (user + assistant messages) */
#define CLAUDE_HISTORY_MAX_MSGS 20

static const char *TAG = "claude";

static char *s_api_key = NULL;
static char *s_model = NULL;
static char *s_system = NULL;
static char *s_effort = NULL;
/* thinking.type sent with each request, NULL to leave the parameter out */
static const char *s_thinking = NULL;
/* Conversation history: user messages and assistant contents built from the stream */
static cJSON *s_messages = NULL;
/* Kept between requests so that the TLS connection can be reused (keep-alive) */
static esp_http_client_handle_t s_client = NULL;

/* ---------- stream state ---------- */

typedef enum {
    BLOCK_NONE,
    BLOCK_TEXT,
    BLOCK_THINKING,
    BLOCK_TOOL_USE,
    BLOCK_OTHER,
} block_type_t;

typedef struct {
    llm_text_cb_t on_text;
    void *ctx;

    llm_sse_t sse;

    llm_buf_t text;                 /* whole answer text */
    llm_buf_t block;                /* current content block (text, thinking or tool input JSON) */
    llm_buf_t signature;            /* signature of the current thinking block */
    block_type_t block_type;
    char tool_id[64];
    char tool_name[64];
    cJSON *content;             /* assistant content blocks, stored in the history */

    char stop_reason[24];
    bool done;                  /* message_stop received */
    llm_error_t error;

    int64_t start_us;
    int64_t first_text_us;
    int input_tokens;
    int cache_read_tokens;
    int cache_write_tokens;
    int output_tokens;
} stream_t;

static void replace_str(char **dst, const char *src)
{
    free(*dst);
    *dst = (src && src[0]) ? strdup(src) : NULL;
}

esp_err_t claude_client_init(const char *api_key, const char *model, const char *system_prompt, const char *effort)
{
    ESP_RETURN_ON_FALSE(api_key && api_key[0] && model && model[0], ESP_ERR_INVALID_ARG, TAG, "API key and model required");
    replace_str(&s_api_key, api_key);
    replace_str(&s_model, model);
    replace_str(&s_system, system_prompt);
    replace_str(&s_effort, effort);
    claude_client_reset();
    /*
     * Haiku accepts "disabled". Sonnet rejects it but only thinks between tool calls with "between_tools"
     * (no tools are offered). Opus cannot turn thinking off: the parameter is left out.
     */
    if (strstr(s_model, "haiku")) {
        s_thinking = "disabled";
    } else if (strstr(s_model, "sonnet")) {
        s_thinking = "between_tools";
    } else {
        s_thinking = NULL;
    }
    ESP_LOGI(TAG, "model: %s, effort: %s, thinking: %s, streaming", s_model, s_effort ? s_effort : "(default)",
             s_thinking ? s_thinking : "(model default)");

    // A key pasted in CONFIG.INI may carry spaces or line endings, which break the HTTP header
    size_t key_len = strlen(s_api_key);
    for (size_t i = 0; i < key_len; i++) {
        if ((unsigned char)s_api_key[i] <= ' ' || (unsigned char)s_api_key[i] >= 0x7f) {
            ESP_LOGW(TAG, "Claude key contains a space or invalid character at position %u (length %u)",
                     (unsigned)i, (unsigned)key_len);
            break;
        }
    }
    if (strncmp(s_api_key, "sk-ant-", 7) != 0) {
        ESP_LOGW(TAG, "Claude key does not start with \"sk-ant-\"");
    }
    return ESP_OK;
}

void claude_client_reset(void)
{
    cJSON_Delete(s_messages);
    s_messages = cJSON_CreateArray();
}

/* Map an API error to the error kinds shown to the user */
static llm_error_t classify_error(int status, const char *type, const char *message)
{
    if (status == 401 || status == 403 || (type && (strcmp(type, "authentication_error") == 0 ||
                                                     strcmp(type, "permission_error") == 0))) {
        return LLM_ERR_AUTH;
    }
    if ((type && strcmp(type, "billing_error") == 0) || (message && strstr(message, "credit balance"))) {
        return LLM_ERR_CREDIT;
    }
    if (status == 429 || status >= 500 || (type && (strcmp(type, "overloaded_error") == 0 ||
                                                     strcmp(type, "rate_limit_error") == 0 ||
                                                     strcmp(type, "api_error") == 0))) {
        return LLM_ERR_BUSY;
    }
    return LLM_ERR_OTHER;
}

static llm_error_t error_from_json(int status, const cJSON *json)
{
    const cJSON *api_error = cJSON_GetObjectItem(json, "error");
    const cJSON *type = cJSON_GetObjectItem(api_error, "type");
    const cJSON *message = cJSON_GetObjectItem(api_error, "message");
    const char *type_str = cJSON_IsString(type) ? type->valuestring : NULL;
    const char *message_str = cJSON_IsString(message) ? message->valuestring : NULL;
    ESP_LOGE(TAG, "HTTP %d: %s: %s", status, type_str ? type_str : "?", message_str ? message_str : "?");
    return classify_error(status, type_str, message_str);
}

static char *build_request(void)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", s_model);
    cJSON_AddNumberToObject(req, "max_tokens", CLAUDE_MAX_TOKENS);
    cJSON_AddBoolToObject(req, "stream", true);
    /* Thinking off when the model allows it: lowest latency to the first spoken word */
    if (s_thinking) {
        cJSON *thinking = cJSON_AddObjectToObject(req, "thinking");
        cJSON_AddStringToObject(thinking, "type", s_thinking);
    }
    if (s_effort) {
        cJSON *output_config = cJSON_AddObjectToObject(req, "output_config");
        cJSON_AddStringToObject(output_config, "effort", s_effort);
    }
    if (s_system) {
        cJSON_AddStringToObject(req, "system", s_system);
    }
    /*
     * Automatic prompt caching: the breakpoint moves to the last cacheable block. The system prompt
     * alone is below the model's minimum cacheable size; the growing conversation reaches it.
     */
    cJSON *cache_control = cJSON_AddObjectToObject(req, "cache_control");
    cJSON_AddStringToObject(cache_control, "type", "ephemeral");
    /* Referenced, not copied: the history stays owned by s_messages */
    cJSON_AddItemReferenceToObject(req, "messages", s_messages);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return body;
}

/* ---------- SSE event handling ---------- */

static void on_block_start(stream_t *st, const cJSON *block)
{
    const cJSON *type = cJSON_GetObjectItem(block, "type");
    llm_buf_clear(&st->block);
    llm_buf_clear(&st->signature);
    st->block_type = BLOCK_OTHER;
    if (!cJSON_IsString(type)) {
        return;
    }
    if (strcmp(type->valuestring, "text") == 0) {
        st->block_type = BLOCK_TEXT;
    } else if (strcmp(type->valuestring, "thinking") == 0) {
        st->block_type = BLOCK_THINKING;
    } else if (strcmp(type->valuestring, "redacted_thinking") == 0) {
        /* Complete in the start event; sent back unchanged with the history */
        cJSON_AddItemToArray(st->content, cJSON_Duplicate(block, true));
    } else if (strcmp(type->valuestring, "tool_use") == 0) {
        st->block_type = BLOCK_TOOL_USE;
        const cJSON *id = cJSON_GetObjectItem(block, "id");
        const cJSON *name = cJSON_GetObjectItem(block, "name");
        strlcpy(st->tool_id, cJSON_IsString(id) ? id->valuestring : "", sizeof(st->tool_id));
        strlcpy(st->tool_name, cJSON_IsString(name) ? name->valuestring : "", sizeof(st->tool_name));
    }
}

static void on_block_delta(stream_t *st, const cJSON *delta)
{
    const cJSON *type = cJSON_GetObjectItem(delta, "type");
    if (!cJSON_IsString(type)) {
        return;
    }
    if (strcmp(type->valuestring, "text_delta") == 0) {
        const cJSON *text = cJSON_GetObjectItem(delta, "text");
        if (cJSON_IsString(text) && text->valuestring[0]) {
            size_t n = strlen(text->valuestring);
            llm_buf_append(&st->block, text->valuestring, n);
            llm_buf_append(&st->text, text->valuestring, n);
            if (!st->first_text_us) {
                st->first_text_us = esp_timer_get_time();
            }
            if (st->on_text) {
                st->on_text(text->valuestring, st->ctx);
            }
        }
    } else if (strcmp(type->valuestring, "thinking_delta") == 0) {
        const cJSON *thinking = cJSON_GetObjectItem(delta, "thinking");
        if (cJSON_IsString(thinking)) {
            llm_buf_append(&st->block, thinking->valuestring, strlen(thinking->valuestring));
        }
    } else if (strcmp(type->valuestring, "signature_delta") == 0) {
        const cJSON *signature = cJSON_GetObjectItem(delta, "signature");
        if (cJSON_IsString(signature)) {
            llm_buf_append(&st->signature, signature->valuestring, strlen(signature->valuestring));
        }
    } else if (strcmp(type->valuestring, "input_json_delta") == 0) {
        const cJSON *partial = cJSON_GetObjectItem(delta, "partial_json");
        if (cJSON_IsString(partial)) {
            llm_buf_append(&st->block, partial->valuestring, strlen(partial->valuestring));
        }
    }
}

static void on_block_stop(stream_t *st)
{
    if (st->block_type == BLOCK_TEXT && st->block.len > 0) {
        cJSON *block = cJSON_CreateObject();
        cJSON_AddStringToObject(block, "type", "text");
        cJSON_AddStringToObject(block, "text", st->block.p);
        cJSON_AddItemToArray(st->content, block);
    } else if (st->block_type == BLOCK_THINKING) {
        /* Sent back verbatim with the history (the API checks the signature) */
        cJSON *block = cJSON_CreateObject();
        cJSON_AddStringToObject(block, "type", "thinking");
        cJSON_AddStringToObject(block, "thinking", st->block.len ? st->block.p : "");
        cJSON_AddStringToObject(block, "signature", st->signature.len ? st->signature.p : "");
        cJSON_AddItemToArray(st->content, block);
    } else if (st->block_type == BLOCK_TOOL_USE) {
        cJSON *input = cJSON_Parse(st->block.len ? st->block.p : "{}");
        cJSON *block = cJSON_CreateObject();
        cJSON_AddStringToObject(block, "type", "tool_use");
        cJSON_AddStringToObject(block, "id", st->tool_id);
        cJSON_AddStringToObject(block, "name", st->tool_name);
        cJSON_AddItemToObject(block, "input", input ? input : cJSON_CreateObject());
        cJSON_AddItemToArray(st->content, block);
    }
    st->block_type = BLOCK_NONE;
    llm_buf_clear(&st->block);
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

    if (strcmp(t, "content_block_delta") == 0) {
        on_block_delta(st, cJSON_GetObjectItem(json, "delta"));
    } else if (strcmp(t, "content_block_start") == 0) {
        on_block_start(st, cJSON_GetObjectItem(json, "content_block"));
    } else if (strcmp(t, "content_block_stop") == 0) {
        on_block_stop(st);
    } else if (strcmp(t, "message_start") == 0) {
        const cJSON *usage = cJSON_GetObjectItem(cJSON_GetObjectItem(json, "message"), "usage");
        const cJSON *v;
        if ((v = cJSON_GetObjectItem(usage, "input_tokens")) && cJSON_IsNumber(v)) {
            st->input_tokens = v->valueint;
        }
        if ((v = cJSON_GetObjectItem(usage, "cache_read_input_tokens")) && cJSON_IsNumber(v)) {
            st->cache_read_tokens = v->valueint;
        }
        if ((v = cJSON_GetObjectItem(usage, "cache_creation_input_tokens")) && cJSON_IsNumber(v)) {
            st->cache_write_tokens = v->valueint;
        }
    } else if (strcmp(t, "message_delta") == 0) {
        const cJSON *stop = cJSON_GetObjectItem(cJSON_GetObjectItem(json, "delta"), "stop_reason");
        if (cJSON_IsString(stop)) {
            strlcpy(st->stop_reason, stop->valuestring, sizeof(st->stop_reason));
        }
        const cJSON *out = cJSON_GetObjectItem(cJSON_GetObjectItem(json, "usage"), "output_tokens");
        if (cJSON_IsNumber(out)) {
            st->output_tokens = out->valueint;
        }
    } else if (strcmp(t, "message_stop") == 0) {
        st->done = true;
    } else if (strcmp(t, "error") == 0) {
        /* Error sent inside the stream (e.g. overloaded_error) */
        st->error = error_from_json(200, json);
        st->done = true;
    }
    /* "ping" and unknown event types are ignored */
    cJSON_Delete(json);
}

/* ---------- HTTP ---------- */

static esp_http_client_handle_t get_client(void)
{
    if (!s_client) {
        esp_http_client_config_t config = {
            .url = CLAUDE_URL,
            .method = HTTP_METHOD_POST,
            .timeout_ms = CLAUDE_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = true,
        };
        s_client = esp_http_client_init(&config);
        if (s_client) {
            esp_http_client_set_header(s_client, "Content-Type", "application/json");
            esp_http_client_set_header(s_client, "anthropic-version", CLAUDE_API_VERSION);
            esp_http_client_set_header(s_client, "Accept", "text/event-stream");
        }
    }
    if (s_client) {
        esp_http_client_set_header(s_client, "x-api-key", s_api_key);
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

void claude_client_prewarm(void)
{
    if (!s_api_key) {
        return;
    }
    int64_t t0 = esp_timer_get_time();
    esp_http_client_handle_t client = get_client();
    if (!client) {
        return;
    }
    /* A free request with a tiny answer (one model): it opens the TLS connection, kept alive for the question */
    esp_http_client_set_url(client, CLAUDE_PREWARM_URL);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    int status = -1;
    if (esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
        status = esp_http_client_get_status_code(client);
        /*
         * Read the body to its end: esp_http_client_flush_response() can leave a small body that came
         * with the headers in the connection, where it would be read as the start of the next response
         */
        char discard[256];
        while (esp_http_client_read(client, discard, sizeof(discard)) > 0) {
        }
    }
    if (status == 200 && esp_http_client_is_complete_data_received(client)) {
        esp_http_client_set_url(client, CLAUDE_URL);
        esp_http_client_set_method(client, HTTP_METHOD_POST);
    } else {
        drop_client();
    }
    ESP_LOGI(TAG, "prewarm: HTTP %d in %lld ms", status, (esp_timer_get_time() - t0) / 1000);
}

/* Keep the history short; older exchanges are dropped in user/assistant pairs */
static void trim_history(void)
{
    while (cJSON_GetArraySize(s_messages) > CLAUDE_HISTORY_MAX_MSGS) {
        cJSON_DeleteItemFromArray(s_messages, 0);
        cJSON_DeleteItemFromArray(s_messages, 0);
    }
}

const cJSON *claude_client_history(void)
{
    return s_messages;
}

void claude_client_set_history(cJSON *messages)
{
    cJSON_Delete(s_messages);
    s_messages = messages;
    trim_history();
}

/*
 * Tools are not offered yet, so Claude should never stop with "tool_use". The tool calls are
 * logged here; this is where they will be executed once tools are added.
 */
static void handle_tool_use(const cJSON *content)
{
    const cJSON *block;
    cJSON_ArrayForEach(block, content) {
        const cJSON *type = cJSON_GetObjectItem(block, "type");
        if (cJSON_IsString(type) && strcmp(type->valuestring, "tool_use") == 0) {
            char *input = cJSON_PrintUnformatted(cJSON_GetObjectItem(block, "input"));
            ESP_LOGW(TAG, "Tool call not supported yet: %s(%s)",
                     cJSON_GetObjectItem(block, "name")->valuestring, input ? input : "");
            free(input);
        }
    }
}

llm_error_t claude_client_stream(const char *text, llm_text_cb_t on_text, void *ctx, char **full_text)
{
    if (full_text) {
        *full_text = NULL;
    }
    if (!s_api_key || !s_messages) {
        return LLM_ERR_NO_KEY;
    }

    stream_t st = {
        .on_text = on_text,
        .ctx = ctx,
        .sse = { .on_event = sse_dispatch },
        .content = cJSON_CreateArray(),
        .error = LLM_OK,
        .start_us = esp_timer_get_time(),
    };
    int status = 0;
    char *body = NULL;
    st.sse.ctx = &st;

    /* Append the question; removed again if the exchange fails */
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", text);
    cJSON_AddItemToArray(s_messages, user_msg);

    body = build_request();
    if (!body) {
        st.error = LLM_ERR_OTHER;
        goto end;
    }
    if (send_request(body, &status) != ESP_OK) {
        st.error = LLM_ERR_NETWORK;
        goto end;
    }

    char chunk[CLAUDE_READ_CHUNK];
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
        ESP_LOGE(TAG, "Stream ended before message_stop");
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
        if (strcmp(st.stop_reason, "refusal") == 0) {
            ESP_LOGW(TAG, "Request declined (stop_reason: refusal)");
            st.error = LLM_ERR_REFUSAL;
        } else if (strcmp(st.stop_reason, "tool_use") == 0) {
            handle_tool_use(st.content);
            /* Not stored: the history would need a tool_result for this turn */
            if (st.text.len == 0) {
                st.error = LLM_ERR_OTHER;
            } else {
                cJSON_DeleteItemFromArray(s_messages, cJSON_GetArraySize(s_messages) - 1);
                user_msg = NULL;
            }
        } else if (strcmp(st.stop_reason, "max_tokens") == 0) {
            ESP_LOGW(TAG, "Answer truncated (max_tokens)");
        }
        if (st.error == LLM_OK && st.text.len == 0) {
            ESP_LOGE(TAG, "No text in answer (stop_reason: %s)", st.stop_reason);
            st.error = LLM_ERR_OTHER;
        }
    }

    int64_t now = esp_timer_get_time();
    ESP_LOGI(TAG, "first text after %lld ms, total %lld ms, stop: %s, tokens in %d (cache read %d, write %d), out %d",
             st.first_text_us ? (st.first_text_us - st.start_us) / 1000 : -1LL, (now - st.start_us) / 1000,
             st.stop_reason[0] ? st.stop_reason : "-", st.input_tokens, st.cache_read_tokens,
             st.cache_write_tokens, st.output_tokens);

end:
    if (st.error == LLM_OK) {
        if (user_msg) {
            cJSON *assistant_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(assistant_msg, "role", "assistant");
            cJSON_AddItemToObject(assistant_msg, "content", st.content);
            st.content = NULL;
            cJSON_AddItemToArray(s_messages, assistant_msg);
            trim_history();
        }
        if (full_text) {
            *full_text = st.text.p;
            st.text.p = NULL;
        }
    } else if (user_msg) {
        /* Drop the unanswered question so that the history keeps alternating user/assistant */
        cJSON_DeleteItemFromArray(s_messages, cJSON_GetArraySize(s_messages) - 1);
    }
    cJSON_Delete(st.content);
    llm_sse_free(&st.sse);
    llm_buf_free(&st.block);
    llm_buf_free(&st.signature);
    llm_buf_free(&st.text);
    free(body);
    return st.error;
}
