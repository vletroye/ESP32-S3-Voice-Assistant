/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Mistral AI client (Mistral Small, Medium, Large...): its Conversations API (POST /v1/conversations), with
 * Mistral's web search (the "web_search" tool, run by Mistral) and the device's tools as functions; a model
 * refusing that API is asked through Chat Completions (ai_provider_chat_completions.c, the device's tools only),
 * until another model. Both keep the conversation as [{role, content}]: it goes from one to the other.
 *
 * Conversations, stateless (store: false): the whole history is sent as "inputs" with each request, the question
 * as {"type": "message.input", "role": "user", "content"}. The stream's events: "message.output.delta" (text, or
 * chunks: text, thinking, tool_reference for the sources), "tool.execution.started" (a web search),
 * "function.call.delta" (a function called: tool_call_id, name, arguments), "conversation.response.done" (usage),
 * "conversation.response.error". The results of the functions go back as "function.call" and "function.result"
 * entries after the text said before the calls ("message.output").
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "ai_provider_mistral.h"
#include "ai_provider_chat_completions.h"
#include "file_costs.h"
#include "llm_common.h"

static const char *TAG = "mistral";

#define MISTRAL_CONV_URL        "https://api.mistral.ai/v1/conversations"
/* A tiny free answer (the model's card): opens the TLS connection in advance */
#define MISTRAL_PREWARM_URL     "https://api.mistral.ai/v1/models/%s"
#define MISTRAL_WEB_SEARCH_COST "mistral:web_search"

/*
 * Instead of LLM_WEB_SEARCH_HINT: asked to say a sentence before searching ("Je regarde."), Mistral's models often
 * ended their answer with it, without searching
 */
#define MISTRAL_WEB_SEARCH_HINT "You can search the web for current information (news, results, schedules, " \
                                "prices, opening hours...): search at once when needed, without announcing it. " \
                                "Never read out web addresses, citations or source names unless asked."

/* Function calls of a round, and rounds of calls answered per question (the last one asked without them) */
#define MC_TOOL_CALLS_MAX   8
#define MC_TOOL_ROUNDS_MAX  5

static const char *const s_credit_words[] = { "billing", "payment", "quota", NULL };

/*
 * Chat Completions, the fallback: OpenAI-like; Mistral rejects the fields it does not know: no stream_options (the
 * usage comes with the last chunk anyway). Its reasoning models give their answer as parts (thinking, text), and a
 * reasoning effort it refuses is reported with the values it takes. Also the errors of the Conversations API.
 */
static const cc_config_t s_cc_config = {
    .tag = "mistral",
    .url = "https://api.mistral.ai/v1/chat/completions",
    .prewarm_url = MISTRAL_PREWARM_URL,
    .reasoning = CC_REASONING_EFFORT,
    .usage = CC_USAGE_LAST_CHUNK,
    .credit_words = s_credit_words,
    // Function calling: the device's tools (weather, radio, volume, Homey)
    .tools = true,
};

static cc_client_t s_cc = { .config = &s_cc_config };
/* The model refused the Conversations API: Chat Completions, until another model */
static bool s_chat_completions = false;

/* The Conversations client */
static struct {
    llm_http_t http;
    char *auth;                 /* "Bearer <key>" */
    char *model;
    char *instructions;
    char *effort;
    char prewarm_url[160];
    bool web_refused;           /* the model refused the web search: asked without it, until another model */
    bool api_refused;           /* the model refused the Conversations API: Chat Completions */
    bool tools_off;             /* the last round of a question: no more calls (tool_choice none), an answer is due */
    const cJSON *tools;
    llm_tool_runner_t runner;
    /* Conversation history: [{role, content}] at rest, the entries of the tool rounds while a question is asked */
    cJSON *messages;
} s_mc;

/* A round: one request and its streamed answer (its text added to the answer of the question) */
typedef struct {
    llm_answer_t *answer;
    llm_sse_t sse;
    llm_buf_t text;             /* the text of this round, given back before its calls */
    struct {
        char id[64];
        char name[64];
        llm_buf_t args;
    } calls[MC_TOOL_CALLS_MAX];
    int call_count;
    int web_searches;           /* web searches made by Mistral (billed per call) */
    bool done;                  /* conversation.response.done / error received */
    bool retry;                 /* to be sent again (the web search refused) */
    llm_error_t error;
    int input_tokens;
    int connector_tokens;       /* the search results read by the model: billed as input */
    int output_tokens;
} stream_t;

/* ---------- request ---------- */

static char *build_request(void)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", s_mc.model);
    cJSON_AddBoolToObject(req, "stream", true);
    cJSON_AddBoolToObject(req, "store", false);
    char *with_hint = NULL;
    if (!s_mc.web_refused) {
        asprintf(&with_hint, "%s%s%s", s_mc.instructions ? s_mc.instructions : "", s_mc.instructions ? "\n\n" : "",
                 MISTRAL_WEB_SEARCH_HINT);
    }
    if (with_hint || s_mc.instructions) {
        cJSON_AddStringToObject(req, "instructions", with_hint ? with_hint : s_mc.instructions);
    }
    free(with_hint);
    cJSON *args = cJSON_AddObjectToObject(req, "completion_args");
    cJSON_AddNumberToObject(args, "max_tokens", llm_max_output_tokens());
    if (s_mc.effort) {
        cJSON_AddStringToObject(args, "reasoning_effort", s_mc.effort);
    }
    if (s_mc.tools_off) {
        cJSON_AddStringToObject(args, "tool_choice", "none");
    }
    cJSON *tools = cJSON_AddArrayToObject(req, "tools");
    if (!s_mc.web_refused) {
        // Mistral's web search: the model decides when to search
        cJSON *search = cJSON_CreateObject();
        cJSON_AddStringToObject(search, "type", "web_search");
        cJSON_AddItemToArray(tools, search);
    }
    // The device's tools, as functions: {"type": "function", "function": {name, description, parameters}} (the
    // definitions referenced, owned by tools.c)
    const cJSON *def;
    cJSON_ArrayForEach(def, s_mc.runner ? s_mc.tools : NULL) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "type", "function");
        cJSON *function = cJSON_AddObjectToObject(tool, "function");
        cJSON_AddItemReferenceToObject(function, "name", cJSON_GetObjectItem(def, "name"));
        cJSON_AddItemReferenceToObject(function, "description", cJSON_GetObjectItem(def, "description"));
        cJSON_AddItemReferenceToObject(function, "parameters", cJSON_GetObjectItem(def, "input_schema"));
        cJSON_AddItemToArray(tools, tool);
    }
    if (cJSON_GetArraySize(tools) == 0) {
        cJSON_DeleteItemFromObject(req, "tools");
    }
    // The history as entries: a message at rest {role, content} as "message.input" (the user's and the
    // assistant's); the entries of this question's tool rounds (they have a type) as they are, referenced
    cJSON *inputs = cJSON_AddArrayToObject(req, "inputs");
    const cJSON *msg;
    cJSON_ArrayForEach(msg, s_mc.messages) {
        if (cJSON_GetObjectItem(msg, "type")) {
            cJSON_AddItemReferenceToArray(inputs, (cJSON *)msg);
            continue;
        }
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "type", "message.input");
        cJSON_AddItemReferenceToObject(entry, "role", cJSON_GetObjectItem(msg, "role"));
        cJSON_AddItemReferenceToObject(entry, "content", cJSON_GetObjectItem(msg, "content"));
        cJSON_AddItemToArray(inputs, entry);
    }
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return body;
}

/* ---------- SSE event handling ---------- */

static void add_text(stream_t *st, const char *text)
{
    llm_answer_add(st->answer, text, strlen(text));
    llm_buf_append(&st->text, text, strlen(text));
}

/* The call of id (its pieces follow one another), else a new one; NULL when there are too many */
static int call_index(stream_t *st, const char *id)
{
    for (int i = 0; i < st->call_count; i++) {
        if (strcmp(st->calls[i].id, id) == 0) {
            return i;
        }
    }
    if (st->call_count >= MC_TOOL_CALLS_MAX) {
        return -1;
    }
    strlcpy(st->calls[st->call_count].id, id, sizeof(st->calls[0].id));
    return st->call_count++;
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
    if (strcmp(t, "message.output.delta") != 0) {
        ESP_LOGD(TAG, "event: %.300s", data);
    }

    if (strcmp(t, "message.output.delta") == 0) {
        // Text, or a chunk: {"type": "text", "text"}; the thinking and the sources (tool_reference) left out
        const cJSON *content = cJSON_GetObjectItem(json, "content");
        if (cJSON_IsString(content)) {
            add_text(st, content->valuestring);
        } else if (cJSON_IsObject(content)) {
            const cJSON *chunk_type = cJSON_GetObjectItem(content, "type");
            const cJSON *chunk_text = cJSON_GetObjectItem(content, "text");
            if (cJSON_IsString(chunk_type) && strcmp(chunk_type->valuestring, "text") == 0 &&
                    cJSON_IsString(chunk_text)) {
                add_text(st, chunk_text->valuestring);
            }
        }
    } else if (strcmp(t, "tool.execution.started") == 0) {
        // A search of Mistral starts: what was said so far ("Je regarde.") is spoken meanwhile, the web icon shown
        const cJSON *name = cJSON_GetObjectItem(json, "name");
        ESP_LOGI(TAG, "tool %s (Mistral's)", cJSON_IsString(name) ? name->valuestring : "?");
        if (cJSON_IsString(name) && strncmp(name->valuestring, "web_search", 10) == 0) {
            st->web_searches++;
            llm_web_search_started();
            if (st->answer->on_text) {
                st->answer->on_text(NULL, st->answer->ctx);
            }
        }
    } else if (strcmp(t, "function.call.delta") == 0) {
        // A function of the device called: its arguments possibly in pieces
        const cJSON *id = cJSON_GetObjectItem(json, "tool_call_id");
        const cJSON *name = cJSON_GetObjectItem(json, "name");
        const cJSON *args = cJSON_GetObjectItem(json, "arguments");
        int i = cJSON_IsString(id) ? call_index(st, id->valuestring) : -1;
        if (i >= 0) {
            if (cJSON_IsString(name) && name->valuestring[0]) {
                strlcpy(st->calls[i].name, name->valuestring, sizeof(st->calls[i].name));
            }
            if (cJSON_IsString(args)) {
                llm_buf_append(&st->calls[i].args, args->valuestring, strlen(args->valuestring));
            }
        }
    } else if (strcmp(t, "conversation.response.done") == 0) {
        const cJSON *usage = cJSON_GetObjectItem(json, "usage");
        const cJSON *v;
        if ((v = cJSON_GetObjectItem(usage, "prompt_tokens")) && cJSON_IsNumber(v)) {
            st->input_tokens = v->valueint;
        }
        if ((v = cJSON_GetObjectItem(usage, "completion_tokens")) && cJSON_IsNumber(v)) {
            st->output_tokens = v->valueint;
        }
        if ((v = cJSON_GetObjectItem(usage, "connector_tokens")) && cJSON_IsNumber(v)) {
            st->connector_tokens = v->valueint;
        }
        st->done = true;
    } else if (strcmp(t, "conversation.response.error") == 0) {
        // {"message", "code"}: read as an error of Mistral's Chat Completions
        st->error = cc_error_from_json(&s_cc_config, 200, json);
        st->done = true;
    }
    /* Other events (response.started, tool.execution.delta / done...) are ignored */
    cJSON_Delete(json);
}

/* ---------- the question ---------- */

/* One request on the current history, its answer streamed into st */
static void stream_once(stream_t *st)
{
    char *body = build_request();
    int status = 0;
    if (!body) {
        st->error = LLM_ERR_OTHER;
        return;
    }
    if (llm_http_send(&s_mc.http, body, &status) != ESP_OK) {
        free(body);
        st->error = LLM_ERR_NETWORK;
        return;
    }
    free(body);
    if (status != 200) {
        llm_buf_t err_body = {0};
        st->error = llm_http_error(&s_mc.http, status, cc_error_from_json, &s_cc_config, &err_body);
        if (status == 400 && s_mc.effort && err_body.len && cc_effort_refused(s_mc.effort, err_body.p)) {
            st->error = LLM_ERR_EFFORT;
        } else if ((status == 400 || status == 422) && err_body.len && !s_mc.web_refused &&
                   strstr(err_body.p, "web_search")) {
            ESP_LOGW(TAG, "%s refuses the web search: answered without it", s_mc.model);
            s_mc.web_refused = true;
            st->retry = true;
        } else if ((status == 400 || status == 404 || status == 422) && st->error == LLM_ERR_OTHER) {
            // A model the Conversations API does not take (or a request it does not understand): Chat Completions
            ESP_LOGW(TAG, "%s refuses the Conversations API (HTTP %d): Chat Completions", s_mc.model, status);
            s_mc.api_refused = true;
        }
        llm_buf_free(&err_body);
        return;
    }
    llm_http_stream(&s_mc.http, &st->sse, &st->done);
    if (st->error == LLM_OK && !st->done) {
        ESP_LOGE(TAG, "Stream ended before conversation.response.done");
        st->error = LLM_ERR_NETWORK;
    }
    llm_http_finish(&s_mc.http, st->error == LLM_OK);
    ESP_LOGI(TAG, "round: %d function call(s), %d web search(es), tokens in %d (search results %d), out %d",
             st->call_count, st->web_searches, st->input_tokens, st->connector_tokens, st->output_tokens);
    // Each round is billed (no cached tokens reported), the search results as input
    costs_add_tokens(COST_LLM, s_mc.model, st->input_tokens + st->connector_tokens, 0, 0, st->output_tokens);
    costs_add_calls(MISTRAL_WEB_SEARCH_COST, st->web_searches);
}

/*
 * The functions the model called: the text said before them ("message.output"), each call ("function.call") and
 * each result ("function.result"), added to the history (only for the rounds of this question: the history keeps
 * the question and the final answer)
 */
static void run_tools(stream_t *st)
{
    if (st->text.len) {
        cJSON *said = cJSON_CreateObject();
        cJSON_AddStringToObject(said, "type", "message.output");
        cJSON_AddStringToObject(said, "role", "assistant");
        cJSON_AddStringToObject(said, "content", st->text.p);
        cJSON_AddItemToArray(s_mc.messages, said);
    }
    for (int i = 0; i < st->call_count; i++) {
        const char *args = st->calls[i].args.len ? st->calls[i].args.p : "{}";
        cJSON *call = cJSON_CreateObject();
        cJSON_AddStringToObject(call, "type", "function.call");
        cJSON_AddStringToObject(call, "tool_call_id", st->calls[i].id);
        cJSON_AddStringToObject(call, "name", st->calls[i].name);
        cJSON_AddStringToObject(call, "arguments", args);
        cJSON_AddItemToArray(s_mc.messages, call);
    }
    for (int i = 0; i < st->call_count; i++) {
        cJSON *input = cJSON_Parse(st->calls[i].args.len ? st->calls[i].args.p : "{}");
        if (!cJSON_IsObject(input)) {
            cJSON_Delete(input);
            input = cJSON_CreateObject();   // arguments not readable: none
        }
        bool is_error = false;
        ESP_LOGI(TAG, "tool %s(%.200s)", st->calls[i].name, st->calls[i].args.len ? st->calls[i].args.p : "");
        char *output = s_mc.runner(st->calls[i].name, input, &is_error);
        cJSON_Delete(input);
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "type", "function.result");
        cJSON_AddStringToObject(result, "tool_call_id", st->calls[i].id);
        cJSON_AddStringToObject(result, "result", output ? output : "{\"error\":\"tool unavailable\"}");
        cJSON_AddItemToArray(s_mc.messages, result);
        free(output);
    }
}

static void stream_free(stream_t *st)
{
    llm_sse_free(&st->sse);
    llm_buf_free(&st->text);
    for (int i = 0; i < MC_TOOL_CALLS_MAX; i++) {
        llm_buf_free(&st->calls[i].args);
    }
    free(st);
}

static llm_error_t mc_stream(const char *text, llm_text_cb_t on_text, void *ctx, char **full_text)
{
    if (!s_mc.auth || !s_mc.messages) {
        return LLM_ERR_NO_KEY;
    }
    llm_answer_t answer;
    llm_answer_begin(&answer, on_text, ctx);
    llm_error_t error = LLM_OK;
    int answer_tokens = 0;
    int web_searches = 0;

    /* Append the question; the entries of this exchange are removed again if it fails */
    int start_count = cJSON_GetArraySize(s_mc.messages);
    llm_history_add(s_mc.messages, "user", text);
    s_mc.tools_off = false;

    for (int round = 0; ; round++) {
        // In PSRAM: the function calls take room, and the caller's stack (the SR handler task) is short
        stream_t *st = heap_caps_calloc(1, sizeof(stream_t), MALLOC_CAP_SPIRAM);
        if (!st) {
            error = LLM_ERR_OTHER;
            break;
        }
        st->answer = &answer;
        st->sse.on_event = sse_dispatch;
        st->sse.ctx = st;
        st->error = LLM_OK;
        stream_once(st);
        llm_answer_end(&answer);
        if (st->retry && round == 0) {
            stream_free(st);
            round = -1;         // the same question, without the web search
            continue;
        }
        error = st->error;
        answer_tokens += st->output_tokens;
        web_searches += st->web_searches;
        if (error != LLM_OK || s_mc.api_refused || st->call_count == 0 || !s_mc.runner) {
            stream_free(st);
            break;
        }
        if (round >= MC_TOOL_ROUNDS_MAX) {
            ESP_LOGE(TAG, "Function call not answered (round %d)", round + 1);
            stream_free(st);
            error = LLM_ERR_OTHER;
            break;
        }
        // What was said before the call ("Je regarde.") is spoken while the tools run
        if (on_text) {
            on_text(NULL, ctx);
        }
        run_tools(st);
        stream_free(st);
        // Still calling after the rounds allowed: the last one without calls, it answers with what it has
        s_mc.tools_off = (round + 1 >= MC_TOOL_ROUNDS_MAX);
    }
    s_mc.tools_off = false;

    if (error == LLM_OK && !s_mc.api_refused && answer.text.len == 0) {
        ESP_LOGE(TAG, "No text in answer");
        error = LLM_ERR_OTHER;
    }
    ESP_LOGI(TAG, "first text after %d ms, total %d ms, %d web search(es)", llm_answer_first_ms(&answer),
             llm_answer_total_ms(&answer), web_searches);
    llm_answer_stats(&answer, answer_tokens, -1);

    // The history keeps the question and the final answer: the calls and their results (used already) would be
    // sent again with every later question
    llm_history_drop_from(s_mc.messages, start_count + (error == LLM_OK && !s_mc.api_refused));
    if (error == LLM_OK && !s_mc.api_refused) {
        llm_history_add(s_mc.messages, "assistant", answer.text.p);
        llm_history_trim(s_mc.messages, NULL);
        if (full_text) {
            *full_text = answer.text.p;
            answer.text.p = NULL;
        }
    }
    llm_buf_free(&answer.text);
    return error;
}

/* ---------- the backend: Conversations, Chat Completions for a model refusing it ---------- */

static esp_err_t mistral_init(void *state, const char *api_key, const char *model, const char *instructions,
                              const char *effort)
{
    ESP_RETURN_ON_FALSE(api_key && api_key[0] && model && model[0], ESP_ERR_INVALID_ARG, TAG,
                        "API key and model required");
    s_chat_completions = false;
    llm_str_set(&s_mc.model, model);
    ESP_RETURN_ON_FALSE(llm_bearer_set(&s_mc.auth, api_key), ESP_ERR_NO_MEM, TAG, "no memory");
    llm_str_set(&s_mc.instructions, instructions);
    // "-" in AGENTS.INI leaves the reasoning out, for the models that reject it
    llm_str_set(&s_mc.effort, (effort && strcmp(effort, "-") != 0) ? effort : NULL);
    snprintf(s_mc.prewarm_url, sizeof(s_mc.prewarm_url), MISTRAL_PREWARM_URL, s_mc.model);
    s_mc.http.tag = TAG;
    s_mc.http.url = MISTRAL_CONV_URL;
    s_mc.http.key_header = "Authorization";
    s_mc.http.key = s_mc.auth;
    s_mc.web_refused = false;
    s_mc.api_refused = false;
    llm_history_reset(&s_mc.messages);
    return cc_init(&s_cc, api_key, model, instructions, effort);
}

static void mistral_set_instructions(void *state, const char *instructions)
{
    llm_str_set(&s_mc.instructions, instructions);
    cc_set_instructions(&s_cc, instructions);
}

static void mistral_reset(void *state)
{
    llm_history_reset(&s_mc.messages);
    cc_reset(&s_cc);
}

static const cJSON *mistral_history(void *state)
{
    return s_chat_completions ? cc_history(&s_cc) : s_mc.messages;
}

static void mistral_set_history(void *state, cJSON *messages)
{
    if (s_chat_completions) {
        cc_set_history(&s_cc, messages);
    } else {
        llm_history_set(&s_mc.messages, messages);
    }
}

static void mistral_prewarm(void *state)
{
    if (s_chat_completions) {
        cc_prewarm(&s_cc);
    } else {
        llm_http_prewarm(&s_mc.http, s_mc.prewarm_url, NULL, 0);
    }
}

static void mistral_set_tools(void *state, const cJSON *definitions, llm_tool_runner_t runner)
{
    s_mc.tools = definitions;
    s_mc.runner = definitions ? runner : NULL;
    cc_set_tools(&s_cc, definitions, runner);
}

static llm_error_t mistral_stream(void *state, const char *text, llm_text_cb_t on_text, void *ctx,
                                  char **full_text)
{
    if (full_text) {
        *full_text = NULL;
    }
    if (!s_chat_completions) {
        llm_error_t err = mc_stream(text, on_text, ctx, full_text);
        if (!s_mc.api_refused) {
            return err;
        }
        // Chat Completions from now on, with the conversation so far
        s_chat_completions = true;
        cc_set_history(&s_cc, cJSON_Duplicate(s_mc.messages, true));
    }
    return cc_stream(&s_cc, text, on_text, ctx, full_text);
}

const llm_backend_t llm_backend_mistral = {
    .init = mistral_init,
    .set_instructions = mistral_set_instructions,
    .reset = mistral_reset,
    .history = mistral_history,
    .set_history = mistral_set_history,
    .prewarm = mistral_prewarm,
    .stream = mistral_stream,
    .set_tools = mistral_set_tools,
};
