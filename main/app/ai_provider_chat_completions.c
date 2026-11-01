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
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "cJSON.h"
#include "ai_provider_chat_completions.h"
#include "file_costs.h"


/* Tool calls of a round, and rounds of calls answered per question before giving up */
#define CC_TOOL_CALLS_MAX   8
#define CC_TOOL_ROUNDS_MAX  3

/* A round: one request and its streamed answer (its text added to the answer of the question) */
typedef struct {
    const cc_config_t *config;
    llm_answer_t *answer;
    llm_sse_t sse;
    char finish[24];            /* finish_reason: "stop", "length", "tool_calls", "content_filter"... */
    bool done;                  /* [DONE] or an error event received */
    llm_error_t error;
    /* The tools called, their arguments streamed in pieces */
    struct {
        char id[64];
        char name[64];
        llm_buf_t args;
    } calls[CC_TOOL_CALLS_MAX];
    int call_count;

    int input_tokens;
    int cached_tokens;
    int output_tokens;
    int reasoning_tokens;
    int total_tokens;
    int billed_out;
    bool retry;                 /* the request is to be sent again (the tools refused) */
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
    c->tools_refused = false;
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
    // The device's tools, as functions: {"type": "function", "function": {name, description, parameters}} (the
    // definitions referenced, owned by tools.c)
    if (c->tools && c->runner && !c->tools_refused && cJSON_GetArraySize(c->tools) > 0) {
        cJSON *tools = cJSON_AddArrayToObject(req, "tools");
        const cJSON *def;
        cJSON_ArrayForEach(def, c->tools) {
            cJSON *tool = cJSON_CreateObject();
            cJSON_AddStringToObject(tool, "type", "function");
            cJSON *function = cJSON_AddObjectToObject(tool, "function");
            cJSON_AddItemReferenceToObject(function, "name", cJSON_GetObjectItem(def, "name"));
            cJSON_AddItemReferenceToObject(function, "description", cJSON_GetObjectItem(def, "description"));
            cJSON_AddItemReferenceToObject(function, "parameters", cJSON_GetObjectItem(def, "input_schema"));
            cJSON_AddItemToArray(tools, tool);
        }
        cJSON_AddStringToObject(req, "tool_choice", "auto");
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
        llm_answer_add(st->answer, content->valuestring, strlen(content->valuestring));
    } else if (cJSON_IsArray(content)) {
        // A reasoning model of Mistral: [{"type": "thinking", ...}, {"type": "text", "text": "..."}], the thinking
        // left out
        const cJSON *part;
        cJSON_ArrayForEach(part, content) {
            const cJSON *type = cJSON_GetObjectItem(part, "type");
            const cJSON *part_text = cJSON_GetObjectItem(part, "text");
            if (cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0 && cJSON_IsString(part_text)) {
                llm_answer_add(st->answer, part_text->valuestring, strlen(part_text->valuestring));
            }
        }
    }
    // Tools called: [{"index", "id", "function": {"name", "arguments"}}], the arguments possibly in pieces (the
    // pieces of a call carry its index, or follow its id)
    const cJSON *call;
    cJSON_ArrayForEach(call, cJSON_GetObjectItem(cJSON_GetObjectItem(choice, "delta"), "tool_calls")) {
        const cJSON *index = cJSON_GetObjectItem(call, "index");
        const cJSON *id = cJSON_GetObjectItem(call, "id");
        const cJSON *function = cJSON_GetObjectItem(call, "function");
        const cJSON *name = cJSON_GetObjectItem(function, "name");
        const cJSON *args = cJSON_GetObjectItem(function, "arguments");
        int i = cJSON_IsNumber(index) ? index->valueint : st->call_count - 1;
        if ((!cJSON_IsNumber(index) && cJSON_IsString(id) && id->valuestring[0]) || i < 0) {
            i = st->call_count;     // a new call, without an index
        }
        if (i >= CC_TOOL_CALLS_MAX) {
            continue;
        }
        if (i >= st->call_count) {
            st->call_count = i + 1;
        }
        if (cJSON_IsString(id) && id->valuestring[0]) {
            strlcpy(st->calls[i].id, id->valuestring, sizeof(st->calls[i].id));
        }
        if (cJSON_IsString(name) && name->valuestring[0]) {
            strlcpy(st->calls[i].name, name->valuestring, sizeof(st->calls[i].name));
        }
        if (cJSON_IsString(args)) {
            llm_buf_append(&st->calls[i].args, args->valuestring, strlen(args->valuestring));
        } else if (cJSON_IsObject(args)) {
            char *json = cJSON_PrintUnformatted(args);     // arguments given as an object
            if (json) {
                llm_buf_append(&st->calls[i].args, json, strlen(json));
                free(json);
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

/* One request on the current history, its answer streamed into st (its text added to st->answer) */
static void stream_once(cc_client_t *c, stream_t *st)
{
    const char *tag = c->config->tag;
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
        if (status == 400 && c->effort && err_body.len && effort_refused(c->effort, err_body.p)) {
            st->error = LLM_ERR_EFFORT;
        }
        // "No endpoints found that support tool use" (OpenRouter, 404), "does not support tools" (400): asked again
        // without the tools
        if ((status == 400 || status == 404) && c->tools && !c->tools_refused && err_body.len &&
                (strcasestr(err_body.p, "tool") || strcasestr(err_body.p, "function"))) {
            ESP_LOGW(tag, "%s refuses the tools: answered without them", c->model);
            c->tools_refused = true;
            st->retry = true;
        }
        llm_buf_free(&err_body);
        return;
    }
    llm_http_stream(&c->http, &st->sse, &st->done);
    if (st->error == LLM_OK && !st->done) {
        ESP_LOGE(tag, "Stream ended before [DONE]");
        st->error = LLM_ERR_NETWORK;
    }
    llm_http_finish(&c->http, st->error == LLM_OK);

    ESP_LOGI(tag, "round: finish: %s, %d tool call(s), tokens in %d (cached %d), out %d (reasoning %d)",
             st->finish[0] ? st->finish : "-", st->call_count, st->input_tokens, st->cached_tokens, st->output_tokens,
             st->reasoning_tokens);
    // The reasoning tokens are billed as output; counted in completion_tokens or only in total_tokens, depending
    // on the model: the output billed is what total_tokens holds beyond the prompt. Each round is billed.
    st->billed_out = st->output_tokens;
    if (st->total_tokens > st->input_tokens + st->output_tokens) {
        st->billed_out = st->total_tokens - st->input_tokens;
    }
    // prompt_tokens counts the cached ones
    costs_add_tokens(COST_LLM, c->model, st->input_tokens - st->cached_tokens, st->cached_tokens, 0, st->billed_out);
}

/*
 * The tools the model called: the assistant message holding the calls, then a "tool" message with each result
 * (only for the rounds of this question: the history keeps the question and the final answer)
 */
static void run_tools(cc_client_t *c, const stream_t *st, const char *round_text)
{
    cJSON *calls = cJSON_CreateArray();
    for (int i = 0; i < st->call_count; i++) {
        cJSON *call = cJSON_CreateObject();
        cJSON_AddStringToObject(call, "id", st->calls[i].id);
        cJSON_AddStringToObject(call, "type", "function");
        cJSON *function = cJSON_AddObjectToObject(call, "function");
        cJSON_AddStringToObject(function, "name", st->calls[i].name);
        cJSON_AddStringToObject(function, "arguments", st->calls[i].args.len ? st->calls[i].args.p : "{}");
        cJSON_AddItemToArray(calls, call);
    }
    cJSON *assistant = cJSON_CreateObject();
    cJSON_AddStringToObject(assistant, "role", "assistant");
    cJSON_AddStringToObject(assistant, "content", round_text ? round_text : "");
    cJSON_AddItemToObject(assistant, "tool_calls", calls);
    cJSON_AddItemToArray(c->messages, assistant);
    for (int i = 0; i < st->call_count; i++) {
        cJSON *input = cJSON_Parse(st->calls[i].args.len ? st->calls[i].args.p : "{}");
        if (!cJSON_IsObject(input)) {
            cJSON_Delete(input);
            input = cJSON_CreateObject();   // arguments not readable: none
        }
        bool is_error = false;
        ESP_LOGI(c->config->tag, "tool %s(%.200s)", st->calls[i].name, st->calls[i].args.len ? st->calls[i].args.p : "");
        char *output = c->runner(st->calls[i].name, input, &is_error);
        cJSON_Delete(input);
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "role", "tool");
        cJSON_AddStringToObject(result, "tool_call_id", st->calls[i].id);
        cJSON_AddStringToObject(result, "name", st->calls[i].name);
        cJSON_AddStringToObject(result, "content", output ? output : "{\"error\":\"tool unavailable\"}");
        cJSON_AddItemToArray(c->messages, result);
        free(output);
    }
}

static void stream_free(stream_t *st)
{
    llm_sse_free(&st->sse);
    for (int i = 0; i < CC_TOOL_CALLS_MAX; i++) {
        llm_buf_free(&st->calls[i].args);
    }
}

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

    llm_answer_t answer;
    llm_answer_begin(&answer, on_text, ctx);
    llm_error_t error = LLM_OK;
    int answer_tokens = 0;
    int reasoning_tokens = 0;
    char finish[24] = "";

    /* Append the question; the messages of this exchange are removed again if it fails */
    int start_count = cJSON_GetArraySize(c->messages);
    llm_history_add(c->messages, "user", text);

    for (int round = 0; ; round++) {
        // In PSRAM: the tool calls take more than a KB, and the caller's stack (the SR handler task) is short
        stream_t *st = heap_caps_calloc(1, sizeof(stream_t), MALLOC_CAP_SPIRAM);
        if (!st) {
            error = LLM_ERR_OTHER;
            break;
        }
        st->config = c->config;
        st->answer = &answer;
        st->sse.on_event = sse_dispatch;
        st->sse.ctx = st;
        st->error = LLM_OK;
        size_t text_before = answer.text.len;
        stream_once(c, st);
        if (st->retry && round == 0) {
            stream_free(st);
            free(st);
            round = -1;         // the same question, without the tools
            continue;
        }
        error = st->error;
        strlcpy(finish, st->finish, sizeof(finish));
        answer_tokens += st->billed_out - st->reasoning_tokens;
        reasoning_tokens += st->reasoning_tokens;
        bool tool_calls = error == LLM_OK && st->call_count > 0 && c->runner && strcmp(st->finish, "tool_calls") == 0;
        if (!tool_calls) {
            stream_free(st);
            free(st);
            break;
        }
        if (round >= CC_TOOL_ROUNDS_MAX) {
            ESP_LOGE(tag, "Tool call not answered (round %d)", round + 1);
            stream_free(st);
            free(st);
            error = LLM_ERR_OTHER;
            break;
        }
        // A web search (by Perplexity) shows the web icon; what was said before the tool call ("Je regarde.") is
        // spoken while the tools run
        for (int i = 0; i < st->call_count; i++) {
            if (strcmp(st->calls[i].name, "web_search") == 0) {
                llm_web_search_started();
            }
        }
        if (on_text) {
            on_text(NULL, ctx);
        }
        char *round_text = answer.text.len > text_before ?
                           strndup(answer.text.p + text_before, answer.text.len - text_before) : NULL;
        run_tools(c, st, round_text);
        free(round_text);
        stream_free(st);
        free(st);
    }

    if (error == LLM_OK) {
        if (strcmp(finish, "content_filter") == 0 && answer.text.len == 0) {
            ESP_LOGW(tag, "Request declined (content_filter)");
            error = LLM_ERR_REFUSAL;
        } else if (strcmp(finish, "length") == 0) {
            ESP_LOGW(tag, "Answer truncated (length)");
        }
        if (error == LLM_OK && answer.text.len == 0) {
            ESP_LOGE(tag, "No text in answer (finish: %s)", finish[0] ? finish : "-");
            error = LLM_ERR_OTHER;
        }
    }
    ESP_LOGI(tag, "first text after %d ms, total %d ms, finish: %s", llm_answer_first_ms(&answer),
             llm_answer_total_ms(&answer), finish[0] ? finish : "-");
    llm_answer_stats(&answer, answer_tokens, reasoning_tokens);

    // The history keeps the question and the final answer: the tool calls and their results (used already) would
    // be sent again with every later question
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

void cc_set_tools(void *client, const cJSON *definitions, llm_tool_runner_t runner)
{
    cc_client_t *c = client;
    // Only for a provider whose models take tools (cc_config_t)
    c->tools = c->config->tools ? definitions : NULL;
    c->runner = c->config->tools && definitions ? runner : NULL;
}
