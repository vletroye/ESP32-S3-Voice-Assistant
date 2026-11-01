/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Client of the OpenAI-compatible Chat Completions APIs (OpenRouter, xAI, Mistral AI): see
 * ai_provider_chat_completions.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "cJSON.h"
#include "ai_provider_chat_completions.h"
#include "file_costs.h"


typedef struct {
    const cc_config_t *config;
    llm_answer_t answer;
    llm_sse_t sse;
    char finish[24];            /* finish_reason: "stop", "length", "content_filter", "error"... */
    bool done;                  /* [DONE] or an error event received */
    llm_error_t error;

    int input_tokens;
    int cached_tokens;
    int output_tokens;
    int reasoning_tokens;
    int total_tokens;
} stream_t;

esp_err_t cc_init(void *client, const char *api_key, const char *model, const char *instructions, const char *effort)
{
    cc_client_t *c = client;
    const char *tag = c->config->tag;
    ESP_RETURN_ON_FALSE(api_key && api_key[0] && model && model[0], ESP_ERR_INVALID_ARG, tag,
                        "API key and model required");
    llm_str_set(&c->model, model);
    ESP_RETURN_ON_FALSE(llm_bearer_set(&c->auth, api_key), ESP_ERR_NO_MEM, tag, "no memory");
    llm_str_set(&c->instructions, instructions);
    // "-" in AGENTS.INI leaves the parameter out, for the models that reject it
    llm_str_set(&c->effort, (effort && strcmp(effort, "-") != 0) ? effort : NULL);
    snprintf(c->prewarm_url, sizeof(c->prewarm_url), c->config->prewarm_url, c->model);
    c->http.tag = tag;
    c->http.url = c->config->url;
    c->http.key_header = "Authorization";
    c->http.key = c->auth;
    c->http.headers = c->config->headers;
    cc_reset(c);
    ESP_LOGI(tag, "model: %s, reasoning effort: %s, streaming", c->model, c->effort ? c->effort : "(default)");
    return ESP_OK;
}

void cc_set_instructions(void *client, const char *instructions)
{
    llm_str_set(&((cc_client_t *)client)->instructions, instructions);
}

void cc_reset(void *client)
{
    llm_history_reset(&((cc_client_t *)client)->messages);
}

const cJSON *cc_history(void *client)
{
    return ((cc_client_t *)client)->messages;
}

void cc_set_history(void *client, cJSON *messages)
{
    llm_history_set(&((cc_client_t *)client)->messages, messages);
}

void cc_prewarm(void *client)
{
    cc_client_t *c = client;
    llm_http_prewarm(&c->http, c->prewarm_url, NULL, 0);
}

/* ---------- errors ---------- */

/* Map an API error to the error kinds shown to the user */
static llm_error_t classify_error(const cc_config_t *config, int code, const char *message)
{
    // First: 402, "Insufficient credits" (also said by a provider behind OpenRouter with another code; a team of
    // xAI without credit gets a 403 that says so)
    bool credit = code == 402 || (message && strcasestr(message, "credit"));
    for (const char *const *word = config->credit_words; !credit && message && word && *word; word++) {
        credit = strcasestr(message, *word) != NULL;
    }
    if (credit) {
        return LLM_ERR_CREDIT;
    }
    if (code == 401 || code == 403 || (message && strcasestr(message, "api key"))) {
        return LLM_ERR_AUTH;    // "Incorrect API key provided" (xAI, 400), {"detail": "Invalid API Key"} (Mistral)
    }
    if (code == 408 || code == 429 || code >= 500) {
        return LLM_ERR_BUSY;
    }
    return LLM_ERR_OTHER;
}

/*
 * {"error": {"code", "message"}} (OpenAI style, also in the stream), {"code": "...", "error": "message"} (xAI's
 * HTTP errors), {"object": "error", "message", "code"} (Mistral's errors) or {"detail": "message"} (Mistral's
 * authentication)
 */
static llm_error_t error_from_json(const void *ctx, int status, const cJSON *json)
{
    const cc_config_t *config = ctx;
    if (!json) {
        return classify_error(config, status, NULL);
    }
    const cJSON *api_error = cJSON_GetObjectItem(json, "error");
    const cJSON *message = cJSON_IsString(api_error) ? api_error : NULL;
    if (!cJSON_IsObject(api_error)) {
        api_error = json;
    }
    if (!message) {
        message = cJSON_GetObjectItem(api_error, "message");
    }
    if (!cJSON_IsString(message)) {
        message = cJSON_GetObjectItem(json, "detail");
    }
    const cJSON *code = cJSON_GetObjectItem(api_error, "code");
    int code_num = cJSON_IsNumber(code) ? code->valueint : status;
    const char *text = cJSON_IsString(message) ? message->valuestring : NULL;
    ESP_LOGE(config->tag, "HTTP %d: %d: %s", status, code_num, text ? text : "?");
    return classify_error(config, code_num, text);
}

/*
 * "reasoning_effort low is not supported for this model, supported values: [<ReasoningEffort.high: 'high'>,
 * <ReasoningEffort.none: 'none'>]" (Mistral): the effort of AGENTS.INI is to be corrected; the values it takes,
 * "high, none", in llm_effort_values
 */
static bool effort_refused(const char *effort, const char *body)
{
    const char *values = strstr(body, "reasoning_effort") ? strstr(body, "supported values") : NULL;
    if (!values) {
        return false;
    }
    strlcpy(llm_effort_refused, effort, sizeof(llm_effort_refused));
    llm_effort_values[0] = '\0';
    for (const char *q = strstr(values, ": '"); q; q = strstr(q + 1, ": '")) {
        const char *start = q + 3;
        const char *end = strchr(start, '\'');
        if (!end) {
            break;
        }
        size_t len = strlen(llm_effort_values);
        snprintf(llm_effort_values + len, sizeof(llm_effort_values) - len, "%s%.*s", len ? ", " : "",
                 (int)(end - start), start);
    }
    return true;
}

/* ---------- request ---------- */

static char *build_request(const cc_client_t *c)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", c->model);
    cJSON_AddBoolToObject(req, "stream", true);
    cJSON_AddNumberToObject(req, "max_tokens", llm_max_output_tokens());
    if (c->effort) {
        // Reasoning kept short or left out ("none"), its text not returned: the answer is spoken
        if (c->config->reasoning == CC_REASONING_OBJECT) {
            cJSON *reasoning = cJSON_AddObjectToObject(req, "reasoning");
            cJSON_AddStringToObject(reasoning, "effort", c->effort);
            cJSON_AddBoolToObject(reasoning, "exclude", true);
        } else {
            cJSON_AddStringToObject(req, "reasoning_effort", c->effort);
        }
    }
    if (c->config->usage == CC_USAGE_INCLUDE) {
        cJSON_AddItemToObject(cJSON_AddObjectToObject(req, "usage"), "include", cJSON_CreateTrue());
    } else if (c->config->usage == CC_USAGE_STREAM_OPTIONS) {
        cJSON_AddItemToObject(cJSON_AddObjectToObject(req, "stream_options"), "include_usage", cJSON_CreateTrue());
    }
    /* The instructions, then the history (referenced, not copied: it stays owned by c->messages) */
    cJSON *messages = cJSON_AddArrayToObject(req, "messages");
    if (c->instructions) {
        cJSON *system = cJSON_CreateObject();
        cJSON_AddStringToObject(system, "role", "system");
        cJSON_AddStringToObject(system, "content", c->instructions);
        cJSON_AddItemToArray(messages, system);
    }
    const cJSON *msg;
    cJSON_ArrayForEach(msg, c->messages) {
        cJSON_AddItemReferenceToArray(messages, (cJSON *)msg);
    }
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return body;
}

/* ---------- SSE event handling ---------- */

static int usage_number(const cJSON *usage, const char *details, const char *name)
{
    const cJSON *v = cJSON_GetObjectItem(details ? cJSON_GetObjectItem(usage, details) : usage, name);
    return cJSON_IsNumber(v) ? v->valueint : -1;
}

static void sse_dispatch(const char *data, void *ctx)
{
    stream_t *st = (stream_t *)ctx;
    if (strcmp(data, "[DONE]") == 0) {
        st->done = true;
        return;
    }
    cJSON *json = cJSON_Parse(data);
    if (!json) {
        ESP_LOGW(st->config->tag, "Unparsable event: %.120s", data);
        return;
    }
    const cJSON *api_error = cJSON_GetObjectItem(json, "error");
    const cJSON *object = cJSON_GetObjectItem(json, "object");
    if (cJSON_IsObject(api_error) || cJSON_IsString(api_error) ||
            (cJSON_IsString(object) && strcmp(object->valuestring, "error") == 0)) {
        st->error = error_from_json(st->config, 200, json);
        st->done = true;
        cJSON_Delete(json);
        return;
    }
    const cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "choices"), 0);
    const cJSON *content = cJSON_GetObjectItem(cJSON_GetObjectItem(choice, "delta"), "content");
    if (cJSON_IsString(content)) {
        llm_answer_add(&st->answer, content->valuestring, strlen(content->valuestring));
    } else if (cJSON_IsArray(content)) {
        // A reasoning model of Mistral: [{"type": "thinking", ...}, {"type": "text", "text": "..."}], the thinking
        // left out
        const cJSON *part;
        cJSON_ArrayForEach(part, content) {
            const cJSON *type = cJSON_GetObjectItem(part, "type");
            const cJSON *part_text = cJSON_GetObjectItem(part, "text");
            if (cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0 && cJSON_IsString(part_text)) {
                llm_answer_add(&st->answer, part_text->valuestring, strlen(part_text->valuestring));
            }
        }
    }
    const cJSON *finish = cJSON_GetObjectItem(choice, "finish_reason");
    if (cJSON_IsString(finish)) {
        strlcpy(st->finish, finish->valuestring, sizeof(st->finish));
    }
    const cJSON *usage = cJSON_GetObjectItem(json, "usage");
    int v;
    if ((v = usage_number(usage, NULL, "prompt_tokens")) >= 0) {
        st->input_tokens = v;
    }
    if ((v = usage_number(usage, "prompt_tokens_details", "cached_tokens")) >= 0) {
        st->cached_tokens = v;
    }
    if ((v = usage_number(usage, NULL, "completion_tokens")) >= 0) {
        st->output_tokens = v;
    }
    if ((v = usage_number(usage, "completion_tokens_details", "reasoning_tokens")) >= 0) {
        st->reasoning_tokens = v;
    }
    if ((v = usage_number(usage, NULL, "total_tokens")) >= 0) {
        st->total_tokens = v;
    }
    cJSON_Delete(json);
}

/* ---------- the question ---------- */

llm_error_t cc_stream(void *client, const char *text, llm_text_cb_t on_text, void *ctx, char **full_text)
{
    cc_client_t *c = client;
    const char *tag = c->config->tag;
    if (full_text) {
        *full_text = NULL;
    }
    if (!c->auth || !c->messages) {
        return LLM_ERR_NO_KEY;
    }

    stream_t st = {
        .config = c->config,
        .sse = { .on_event = sse_dispatch },
        .error = LLM_OK,
    };
    st.sse.ctx = &st;
    llm_answer_begin(&st.answer, on_text, ctx);
    int status = 0;
    char *body = NULL;

    /* Append the question; removed again if the exchange fails */
    int start_count = cJSON_GetArraySize(c->messages);
    llm_history_add(c->messages, "user", text);

    body = build_request(c);
    if (!body) {
        st.error = LLM_ERR_OTHER;
        goto end;
    }
    if (llm_http_send(&c->http, body, &status) != ESP_OK) {
        st.error = LLM_ERR_NETWORK;
        goto end;
    }
    if (status != 200) {
        llm_buf_t err_body = {0};
        st.error = llm_http_error(&c->http, status, error_from_json, c->config, &err_body);
        if (status == 400 && c->effort && err_body.len && effort_refused(c->effort, err_body.p)) {
            st.error = LLM_ERR_EFFORT;
        }
        llm_buf_free(&err_body);
        goto end;
    }

    llm_http_stream(&c->http, &st.sse, &st.done);
    if (st.error == LLM_OK && !st.done) {
        ESP_LOGE(tag, "Stream ended before [DONE]");
        st.error = LLM_ERR_NETWORK;
    }
    llm_http_finish(&c->http, st.error == LLM_OK);

    if (st.error == LLM_OK) {
        if (strcmp(st.finish, "content_filter") == 0 && st.answer.text.len == 0) {
            ESP_LOGW(tag, "Request declined (content_filter)");
            st.error = LLM_ERR_REFUSAL;
        } else if (strcmp(st.finish, "length") == 0) {
            ESP_LOGW(tag, "Answer truncated (length)");
        }
        if (st.error == LLM_OK && st.answer.text.len == 0) {
            ESP_LOGE(tag, "No text in answer (finish: %s)", st.finish[0] ? st.finish : "-");
            st.error = LLM_ERR_OTHER;
        }
    }

    ESP_LOGI(tag, "first text after %d ms, total %d ms, finish: %s, tokens in %d (cached %d), out %d (reasoning %d)",
             llm_answer_first_ms(&st.answer), llm_answer_total_ms(&st.answer), st.finish[0] ? st.finish : "-",
             st.input_tokens, st.cached_tokens, st.output_tokens, st.reasoning_tokens);
    // The reasoning tokens are billed as output; counted in completion_tokens or only in total_tokens, depending
    // on the model: the output billed is what total_tokens holds beyond the prompt
    int billed_out = st.output_tokens;
    if (st.total_tokens > st.input_tokens + st.output_tokens) {
        billed_out = st.total_tokens - st.input_tokens;
    }
    llm_answer_stats(&st.answer, billed_out - st.reasoning_tokens, st.reasoning_tokens);
    // prompt_tokens counts the cached ones
    costs_add_tokens(COST_LLM, c->model, st.input_tokens - st.cached_tokens, st.cached_tokens, 0, billed_out);

end:
    if (st.error == LLM_OK) {
        llm_history_add(c->messages, "assistant", st.answer.text.p);
        llm_history_trim(c->messages, NULL);
        if (full_text) {
            *full_text = st.answer.text.p;
            st.answer.text.p = NULL;
        }
    } else {
        /* Drop the unanswered question so that the history keeps alternating user/assistant */
        llm_history_drop_from(c->messages, start_count);
    }
    llm_sse_free(&st.sse);
    llm_buf_free(&st.answer.text);
    free(body);
    return st.error;
}
