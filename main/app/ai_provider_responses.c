/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Client of the Responses APIs (OpenAI, xAI): see ai_provider_responses.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "ai_provider_responses.h"
#include "file_costs.h"
#include "llm_common.h"
#include "settings.h"

/* Function calls of a round, and rounds of calls answered per question before giving up */
#define RC_TOOL_CALLS_MAX   8
#define RC_TOOL_ROUNDS_MAX  3

/* A round: one request and its streamed answer (its text added to the answer of the question) */
typedef struct {
    rc_client_t *client;
    llm_answer_t *answer;
    llm_sse_t sse;
    char status[24];            /* "completed", "incomplete", "failed" */
    bool refused;
    bool done;                  /* response.completed / incomplete / failed received */
    bool retry;                 /* to be sent again (the web search or the reasoning items refused) */
    int web_searches;           /* web searches made (billed per call) */
    llm_error_t error;
    /* The items of the answer (reasoning, function calls...), given back with the results, and the calls */
    cJSON *items;
    struct {
        char call_id[64];
        char name[64];
        llm_buf_t args;
    } calls[RC_TOOL_CALLS_MAX];
    int call_count;

    int input_tokens;
    int cached_tokens;
    int output_tokens;
    int reasoning_tokens;
} stream_t;

void rc_reset(void *state)
{
    rc_client_t *c = state;
    llm_history_reset(&c->messages);
}

esp_err_t rc_init(void *state, const char *api_key, const char *model, const char *instructions, const char *effort)
{
    rc_client_t *c = state;
    const char *TAG = c->config->tag;
    const char *base_url = c->base_url ? c->base_url : c->config->base_url;
    ESP_RETURN_ON_FALSE(base_url && api_key && api_key[0] && model && model[0], ESP_ERR_INVALID_ARG, TAG,
                        "URL, API key and model required");
    const char *sep = base_url[strlen(base_url) - 1] == '/' ? "" : "/";
    snprintf(c->url, sizeof(c->url), "%s%sresponses", base_url, sep);
    llm_str_set(&c->model, model);
    snprintf(c->prewarm_url, sizeof(c->prewarm_url), "%s%smodels/%s", base_url, sep, c->model);
    ESP_RETURN_ON_FALSE(llm_bearer_set(&c->auth, api_key), ESP_ERR_NO_MEM, TAG, "no memory");
    c->http.tag = TAG;
    c->http.url = c->url;
    c->http.key_header = "Authorization";
    c->http.key = c->auth;
    c->api_refused = false;
    c->web_refused = false;
    c->encrypted_refused = false;
    llm_str_set(&c->instructions, instructions);
    // "-" in AGENTS.INI leaves the reasoning out, for the models that reject it
    llm_str_set(&c->effort, (effort && strcmp(effort, "-") != 0) ? effort : NULL);
    rc_reset(c);
    ESP_LOGI(TAG, "model: %s, reasoning effort: %s, streaming", c->model, c->effort ? c->effort : "(default)");
    return ESP_OK;
}

void rc_set_instructions(void *state, const char *instructions)
{
    rc_client_t *c = state;
    llm_str_set(&c->instructions, instructions);
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
    const char *TAG = ((const rc_config_t *)ctx)->tag;
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

static char *build_request(rc_client_t *c)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", c->model);
    cJSON_AddBoolToObject(req, "stream", true);
    cJSON_AddBoolToObject(req, "store", false);
    cJSON_AddNumberToObject(req, "max_output_tokens", llm_max_output_tokens());
    if (c->effort) {
        cJSON *reasoning = cJSON_AddObjectToObject(req, "reasoning");
        cJSON_AddStringToObject(reasoning, "effort", c->effort);
    }
    char *with_hint = c->web_refused ? NULL : llm_with_web_hint(c->instructions);
    if (with_hint || c->instructions) {
        cJSON_AddStringToObject(req, "instructions", with_hint ? with_hint : c->instructions);
    }
    free(with_hint);
    if (!c->web_refused) {
        // The provider's web search; OpenAI's located where the user lives (CONFIG.INI), with little context (short
        // spoken answers)
        cJSON *search = cJSON_CreateObject();
        cJSON_AddStringToObject(search, "type", "web_search");
        const sys_param_t *param = settings_get_parameter();
        if (c->config->search == RC_SEARCH_OPENAI) {
            cJSON_AddStringToObject(search, "search_context_size", "low");
        }
        if (c->config->search == RC_SEARCH_OPENAI && (param->country[0] || param->city[0])) {
            cJSON *where = cJSON_AddObjectToObject(search, "user_location");
            cJSON_AddStringToObject(where, "type", "approximate");
            if (param->country[0]) {
                cJSON_AddStringToObject(where, "country", param->country);
            }
            if (param->city[0]) {
                cJSON_AddStringToObject(where, "city", param->city);
            }
        }
        cJSON_AddItemToArray(cJSON_AddArrayToObject(req, "tools"), search);
    }
    // The device's tools, as functions: {"type": "function", name, description, parameters} (the definitions
    // referenced, owned by tools.c)
    if (c->tools && c->runner && cJSON_GetArraySize(c->tools) > 0) {
        cJSON *tools = cJSON_GetObjectItem(req, "tools");
        if (!tools) {
            tools = cJSON_AddArrayToObject(req, "tools");
        }
        const cJSON *def;
        cJSON_ArrayForEach(def, c->tools) {
            cJSON *tool = cJSON_CreateObject();
            cJSON_AddStringToObject(tool, "type", "function");
            cJSON_AddItemReferenceToObject(tool, "name", cJSON_GetObjectItem(def, "name"));
            cJSON_AddItemReferenceToObject(tool, "description", cJSON_GetObjectItem(def, "description"));
            cJSON_AddItemReferenceToObject(tool, "parameters", cJSON_GetObjectItem(def, "input_schema"));
            cJSON_AddItemToArray(tools, tool);
        }
        // Stateless: the reasoning items come encrypted, to be given back with the results of the calls
        if (c->effort && !c->encrypted_refused) {
            cJSON_AddItemToArray(cJSON_AddArrayToObject(req, "include"),
                                 cJSON_CreateString("reasoning.encrypted_content"));
        }
    }
    /* Referenced, not copied: the history stays owned by c->messages */
    cJSON_AddItemReferenceToObject(req, "input", c->messages);
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
    const char *TAG = st->client->config->tag;
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
            // Its web search adds its sources to the text, " ([site.com](https://...))": taken out (spoken)
            llm_answer_add_cited(st->answer, delta->valuestring, strlen(delta->valuestring));
        }
    } else if (strcmp(t, "response.refusal.delta") == 0) {
        st->refused = true;
    } else if (strcmp(t, "response.web_search_call.in_progress") == 0) {
        // A search starts: what was said so far ("Je regarde.") is spoken meanwhile, the web icon shown
        st->web_searches++;
        llm_web_search_started();
        if (st->answer->on_text) {
            st->answer->on_text(NULL, st->answer->ctx);
        }
    } else if (strcmp(t, "response.output_item.done") == 0) {
        // Each item of the answer kept (given back with the results), its function calls read whole
        const cJSON *item = cJSON_GetObjectItem(json, "item");
        const cJSON *item_type = cJSON_GetObjectItem(item, "type");
        if (cJSON_IsObject(item)) {
            cJSON_AddItemToArray(st->items, cJSON_Duplicate(item, true));
        }
        if (cJSON_IsString(item_type) && strcmp(item_type->valuestring, "function_call") == 0 &&
                st->call_count < RC_TOOL_CALLS_MAX) {
            const cJSON *call_id = cJSON_GetObjectItem(item, "call_id");
            const cJSON *name = cJSON_GetObjectItem(item, "name");
            const cJSON *args = cJSON_GetObjectItem(item, "arguments");
            int i = st->call_count++;
            strlcpy(st->calls[i].call_id, cJSON_IsString(call_id) ? call_id->valuestring : "",
                    sizeof(st->calls[i].call_id));
            strlcpy(st->calls[i].name, cJSON_IsString(name) ? name->valuestring : "", sizeof(st->calls[i].name));
            if (cJSON_IsString(args)) {
                llm_buf_append(&st->calls[i].args, args->valuestring, strlen(args->valuestring));
            }
        }
    } else if (strcmp(t, "response.completed") == 0 || strcmp(t, "response.incomplete") == 0) {
        on_response_end(st, cJSON_GetObjectItem(json, "response"));
    } else if (strcmp(t, "response.failed") == 0) {
        const cJSON *response = cJSON_GetObjectItem(json, "response");
        on_response_end(st, response);
        st->error = error_from_json(st->client->config, 200, response);
    } else if (strcmp(t, "error") == 0) {
        st->error = error_from_json(st->client->config, 200, json);
        st->done = true;
    }
    /* Other events (created, in_progress, output_item, reasoning...) are ignored */
    cJSON_Delete(json);
}

/* ---------- HTTP ---------- */

void rc_prewarm(void *state)
{
    rc_client_t *c = state;
    // A free request with a tiny answer (the model's description): it opens the TLS connection
    llm_http_prewarm(&c->http, c->prewarm_url, NULL, 0);
}

const cJSON *rc_history(void *state)
{
    rc_client_t *c = state;
    return c->messages;
}

void rc_set_history(void *state, cJSON *messages)
{
    rc_client_t *c = state;
    llm_history_set(&c->messages, messages);
}

/* One request on the current history, its answer streamed into st (its text added to st->answer) */
static void stream_once(rc_client_t *c, stream_t *st)
{
    const char *TAG = c->config->tag;
    char *body = build_request(c);
    int status = 0;
    if (!body) {
        st->error = LLM_ERR_OTHER;
        return;
    }
    if (llm_http_send(&c->http, body, &status) != ESP_OK) {
        free(body);
        st->error = LLM_ERR_NETWORK;
        return;
    }
    free(body);
    if (status != 200) {
        llm_buf_t err_body = {0};
        st->error = llm_http_error(&c->http, status, error_from_json, c->config, &err_body);
        if (status == 400 && err_body.len && !c->web_refused &&
                (strstr(err_body.p, "web_search") || strcasestr(err_body.p, "tool"))) {
            // "Web search is not supported with this model / reasoning effort": asked again without it
            ESP_LOGW(TAG, "%s refuses the web search: answered without it", c->model);
            c->web_refused = true;
            st->retry = true;
        } else if (status == 404 || (status == 400 && err_body.len && strcasestr(err_body.p, "not supported") &&
                                     strcasestr(err_body.p, "respons"))) {
            ESP_LOGW(TAG, "%s refuses the Responses API", c->model);
            c->api_refused = true;
        } else if (status == 400 && err_body.len && !c->encrypted_refused && strstr(err_body.p, "encrypted")) {
            // A model without reasoning items to give back: asked again without them
            c->encrypted_refused = true;
            st->retry = true;
        }
        llm_buf_free(&err_body);
        return;
    }
    llm_http_stream(&c->http, &st->sse, &st->done);
    if (st->error == LLM_OK && !st->done) {
        ESP_LOGE(TAG, "Stream ended before response.completed");
        st->error = LLM_ERR_NETWORK;
    }
    llm_http_finish(&c->http, st->error == LLM_OK);
    ESP_LOGI(TAG, "round: status: %s, %d function call(s), tokens in %d (cached %d), out %d (reasoning %d)",
             st->status[0] ? st->status : "-", st->call_count, st->input_tokens, st->cached_tokens, st->output_tokens,
             st->reasoning_tokens);
    // input_tokens counts the cached ones; output_tokens the reasoning (billed too). Each round is billed.
    costs_add_tokens(COST_LLM, c->model, st->input_tokens - st->cached_tokens, st->cached_tokens, 0, st->output_tokens);
    costs_add_calls(c->config->web_search_cost, st->web_searches);
}

/*
 * The functions the model called: the items of its answer given back as they are (reasoning, calls), then a
 * "function_call_output" item with each result (only for the rounds of this question: the history keeps the
 * question and the final answer)
 */
static void run_tools(rc_client_t *c, stream_t *st)
{
    const char *TAG = c->config->tag;
    cJSON *item;
    while ((item = cJSON_DetachItemFromArray(st->items, 0)) != NULL) {
        cJSON_AddItemToArray(c->messages, item);
    }
    for (int i = 0; i < st->call_count; i++) {
        cJSON *input = cJSON_Parse(st->calls[i].args.len ? st->calls[i].args.p : "{}");
        if (!cJSON_IsObject(input)) {
            cJSON_Delete(input);
            input = cJSON_CreateObject();   // arguments not readable: none
        }
        bool is_error = false;
        ESP_LOGI(TAG, "tool %s(%.200s)", st->calls[i].name, st->calls[i].args.len ? st->calls[i].args.p : "");
        char *output = c->runner(st->calls[i].name, input, &is_error);
        cJSON_Delete(input);
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "type", "function_call_output");
        cJSON_AddStringToObject(result, "call_id", st->calls[i].call_id);
        cJSON_AddStringToObject(result, "output", output ? output : "{\"error\":\"tool unavailable\"}");
        cJSON_AddItemToArray(c->messages, result);
        free(output);
    }
}

static void stream_free(stream_t *st)
{
    llm_sse_free(&st->sse);
    for (int i = 0; i < RC_TOOL_CALLS_MAX; i++) {
        llm_buf_free(&st->calls[i].args);
    }
    cJSON_Delete(st->items);
    free(st);
}

llm_error_t rc_stream(void *state, const char *text, llm_text_cb_t on_text, void *ctx, char **full_text)
{
    rc_client_t *c = state;
    const char *TAG = c->config->tag;
    if (full_text) {
        *full_text = NULL;
    }
    if (!c->auth || !c->messages) {
        return LLM_ERR_NO_KEY;
    }

    llm_answer_t answer;
    llm_answer_begin(&answer, on_text, ctx);
    llm_error_t error = LLM_OK;
    int answer_tokens = 0;
    int reasoning_tokens = 0;
    int web_searches = 0;
    char status[24] = "";
    bool refused = false;

    /* Append the question; the items of this exchange are removed again if it fails */
    int start_count = cJSON_GetArraySize(c->messages);
    llm_history_add(c->messages, "user", text);

    for (int round = 0; ; round++) {
        // In PSRAM: the function calls take room, and the caller's stack (the SR handler task) is short
        stream_t *st = heap_caps_calloc(1, sizeof(stream_t), MALLOC_CAP_SPIRAM);
        if (!st) {
            error = LLM_ERR_OTHER;
            break;
        }
        st->client = c;
        st->answer = &answer;
        st->sse.on_event = sse_dispatch;
        st->sse.ctx = st;
        st->error = LLM_OK;
        st->items = cJSON_CreateArray();
        stream_once(c, st);
        llm_answer_end(&answer);    // the text held back while a source link might start
        if (st->retry && round == 0) {
            stream_free(st);
            round = -1;         // the same question, without what the model refused
            continue;
        }
        error = st->error;
        strlcpy(status, st->status, sizeof(status));
        refused |= st->refused;
        answer_tokens += st->output_tokens - st->reasoning_tokens;
        reasoning_tokens += st->reasoning_tokens;
        web_searches += st->web_searches;
        if (error != LLM_OK || st->call_count == 0 || !c->runner) {
            stream_free(st);
            break;
        }
        if (round >= RC_TOOL_ROUNDS_MAX) {
            ESP_LOGE(TAG, "Function call not answered (round %d)", round + 1);
            stream_free(st);
            error = LLM_ERR_OTHER;
            break;
        }
        // What was said before the call ("Je regarde.") is spoken while the tools run
        if (on_text) {
            on_text(NULL, ctx);
        }
        run_tools(c, st);
        stream_free(st);
    }

    if (error == LLM_OK) {
        if (refused && answer.text.len == 0) {
            ESP_LOGW(TAG, "Request declined (refusal)");
            error = LLM_ERR_REFUSAL;
        } else if (strcmp(status, "incomplete") == 0) {
            ESP_LOGW(TAG, "Answer truncated (incomplete)");
        }
        if (error == LLM_OK && answer.text.len == 0) {
            ESP_LOGE(TAG, "No text in answer (status: %s)", status);
            error = LLM_ERR_OTHER;
        }
    }
    ESP_LOGI(TAG, "first text after %d ms, total %d ms, status: %s, %d web search(es)", llm_answer_first_ms(&answer),
             llm_answer_total_ms(&answer), status[0] ? status : "-", web_searches);
    llm_answer_stats(&answer, answer_tokens, reasoning_tokens);

    // The history keeps the question and the final answer: the calls, their results and the reasoning items (used
    // already) would be sent again with every later question
    llm_history_drop_from(c->messages, start_count + (error == LLM_OK));
    if (error == LLM_OK) {
        llm_history_add(c->messages, "assistant", answer.text.p);
        llm_history_trim(c->messages, NULL);
        if (full_text) {
            *full_text = answer.text.p;
            answer.text.p = NULL;
        }
    }
    llm_buf_free(&answer.text);
    return error;
}

void rc_set_tools(void *state, const cJSON *definitions, llm_tool_runner_t runner)
{
    rc_client_t *c = state;
    c->tools = definitions;
    c->runner = definitions ? runner : NULL;
}
