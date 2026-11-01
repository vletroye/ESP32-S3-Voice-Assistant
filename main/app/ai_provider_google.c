/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Gemini client: Gemini API generateContent with server-sent events (streamGenerateContent?alt=sse),
 * the conversation kept on the device and sent with each request. The effort of AGENTS.INI is its
 * thinkingConfig.thinkingLevel ("minimal", "low", ...). With Google Search grounding: the model decides when to
 * search (a model refusing it is asked again without, until another model), and the device's tools as function
 * declarations (weather, volume, radio, Homey).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
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
/* The model refused the Google Search tool: its questions asked without, until another model */
static bool s_search_refused = false;
/* The device's tools (definitions of tools.c) and the function running them, NULL for none */
static const cJSON *s_tools = NULL;
static llm_tool_runner_t s_runner = NULL;
/* Rounds of function calls answered per question before giving up */
#define GEMINI_TOOL_ROUNDS_MAX  5
/* Conversation history: contents [{role: "user" | "model", parts: [{text}]}] */
static cJSON *s_messages = NULL;
/* Kept between requests so that the TLS connection can be reused (keep-alive) */
static llm_http_t s_http = { .url = s_url, .key_header = "x-goog-api-key" };

/* A round: one request and its streamed answer (its text added to the answer of the question) */
typedef struct {
    llm_answer_t *answer;
    llm_sse_t sse;
    cJSON *parts;               /* the parts of the model's content (thought signatures included), given back */
    int call_count;             /* function calls among them */
    bool retry;                 /* to be sent again (Google Search refused) */
    char *signature;            /* last thought signature, sent back with the answer */
    char finish[32];            /* finishReason: "STOP", "MAX_TOKENS", "SAFETY"... */
    bool blocked;               /* prompt blocked (promptFeedback.blockReason) */
    int web_searches;           /* Google searches made (groundingMetadata.webSearchQueries), billed per query */
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
    s_search_refused = false;
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
    char *with_hint = s_search_refused ? NULL : llm_with_web_hint(s_instructions);
    if (with_hint || s_instructions) {
        cJSON *system = cJSON_AddObjectToObject(req, "systemInstruction");
        cJSON *part = cJSON_CreateObject();
        cJSON_AddStringToObject(part, "text", with_hint ? with_hint : s_instructions);
        cJSON_AddItemToObject(system, "parts", cJSON_CreateArray());
        cJSON_AddItemToArray(cJSON_GetObjectItem(system, "parts"), part);
    }
    free(with_hint);
    cJSON *tools = cJSON_CreateArray();
    if (!s_search_refused) {
        // Grounding with Google Search: {"google_search": {}}
        cJSON *search = cJSON_CreateObject();
        cJSON_AddObjectToObject(search, "google_search");
        cJSON_AddItemToArray(tools, search);
    }
    // The device's tools: {"functionDeclarations": [{name, description, parametersJsonSchema}]} (the definitions
    // referenced, owned by tools.c; their schemas given as JSON Schema)
    if (s_tools && s_runner && cJSON_GetArraySize(s_tools) > 0) {
        cJSON *functions = cJSON_CreateObject();
        cJSON *declarations = cJSON_AddArrayToObject(functions, "functionDeclarations");
        const cJSON *def;
        cJSON_ArrayForEach(def, s_tools) {
            cJSON *declaration = cJSON_CreateObject();
            cJSON_AddItemReferenceToObject(declaration, "name", cJSON_GetObjectItem(def, "name"));
            cJSON_AddItemReferenceToObject(declaration, "description", cJSON_GetObjectItem(def, "description"));
            cJSON_AddItemReferenceToObject(declaration, "parametersJsonSchema",
                                           cJSON_GetObjectItem(def, "input_schema"));
            cJSON_AddItemToArray(declarations, declaration);
        }
        cJSON_AddItemToArray(tools, functions);
    }
    if (cJSON_GetArraySize(tools) > 1) {
        // Google Search (run by Google) and the functions (run here) together: Google asks for this setting; the
        // parts of its searches come in the answer, given back with the rest
        cJSON_AddBoolToObject(cJSON_AddObjectToObject(req, "toolConfig"), "includeServerSideToolInvocations", true);
    }
    if (cJSON_GetArraySize(tools) > 0) {
        cJSON_AddItemToObject(req, "tools", tools);
    } else {
        cJSON_Delete(tools);
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
        if (cJSON_IsTrue(cJSON_GetObjectItem(part, "thought"))) {
            continue;   // thought summaries are not spoken
        }
        // Kept as they came: given back with the results of the calls (their thought signatures are checked)
        cJSON_AddItemToArray(st->parts, cJSON_Duplicate(part, true));
        if (cJSON_IsObject(cJSON_GetObjectItem(part, "functionCall"))) {
            st->call_count++;
        }
        const cJSON *text = cJSON_GetObjectItem(part, "text");
        if (cJSON_IsString(text)) {
            llm_answer_add(st->answer, text->valuestring, strlen(text->valuestring));
        }
    }
    // The searches made, given with the grounding of the answer (the last events)
    const cJSON *queries = cJSON_GetObjectItem(cJSON_GetObjectItem(candidate, "groundingMetadata"), "webSearchQueries");
    if (cJSON_IsArray(queries) && cJSON_GetArraySize(queries) > st->web_searches) {
        for (int i = st->web_searches; i < cJSON_GetArraySize(queries); i++) {
            llm_web_search_started();
        }
        st->web_searches = cJSON_GetArraySize(queries);
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

/* One request on the current history, its answer streamed into st (its text added to st->answer) */
static void stream_once(stream_t *st)
{
    char *body = build_request();
    int status = 0;
    if (!body) {
        st->error = LLM_ERR_OTHER;
        return;
    }
    if (llm_http_send(&s_http, body, &status) != ESP_OK) {
        free(body);
        st->error = LLM_ERR_NETWORK;
        return;
    }
    free(body);
    if (status != 200) {
        llm_buf_t err_body = {0};
        st->error = llm_http_error(&s_http, status, error_from_json, NULL, &err_body);
        if (status == 400 && !s_search_refused && err_body.len &&
                (strcasestr(err_body.p, "google_search") || strcasestr(err_body.p, "grounding") ||
                 strcasestr(err_body.p, "tool"))) {
            // Google Search refused by this model, or with the functions: asked again without it
            ESP_LOGW(TAG, "%s refuses Google Search: answered without it", s_model);
            s_search_refused = true;
            st->retry = true;
        }
        llm_buf_free(&err_body);
        return;
    }
    /* The last event carries finishReason; the body ends right after it */
    llm_http_stream(&s_http, &st->sse, NULL);
    if (st->error == LLM_OK && !st->done) {
        ESP_LOGE(TAG, "Stream ended before finishReason");
        st->error = LLM_ERR_NETWORK;
    }
    llm_http_finish(&s_http, st->error == LLM_OK);
    ESP_LOGI(TAG, "round: finish: %s, %d function call(s), tokens in %d (cached %d), out %d (thoughts %d)",
             st->finish[0] ? st->finish : "-", st->call_count, st->input_tokens, st->cached_tokens, st->output_tokens,
             st->thoughts_tokens);
    // promptTokenCount counts the cached ones; the thoughts are billed as output. Each round is billed.
    costs_add_tokens(COST_LLM, s_model, st->input_tokens - st->cached_tokens, st->cached_tokens, 0,
                     st->output_tokens + st->thoughts_tokens);
    costs_add_calls("gemini:web_search", st->web_searches);
}

/*
 * The functions the model called: its content given back as it came (thought signatures included), then a user
 * content with a functionResponse for each call (only for the rounds of this question: the history keeps the
 * question and the final answer)
 */
static void run_tools(stream_t *st)
{
    cJSON *model = cJSON_CreateObject();
    cJSON_AddStringToObject(model, "role", "model");
    cJSON *parts = st->parts;
    st->parts = NULL;
    cJSON_AddItemToObject(model, "parts", parts);
    cJSON_AddItemToArray(s_messages, model);

    cJSON *user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "role", "user");
    cJSON *responses = cJSON_AddArrayToObject(user, "parts");
    const cJSON *part;
    cJSON_ArrayForEach(part, parts) {
        const cJSON *call = cJSON_GetObjectItem(part, "functionCall");
        if (!cJSON_IsObject(call)) {
            continue;
        }
        const cJSON *name = cJSON_GetObjectItem(call, "name");
        const cJSON *id = cJSON_GetObjectItem(call, "id");
        const cJSON *args = cJSON_GetObjectItem(call, "args");
        cJSON *input = cJSON_IsObject(args) ? cJSON_Duplicate(args, true) : cJSON_CreateObject();
        char *args_text = cJSON_PrintUnformatted(input);
        ESP_LOGI(TAG, "tool %s(%.200s)", cJSON_IsString(name) ? name->valuestring : "?", args_text ? args_text : "");
        free(args_text);
        bool is_error = false;
        char *output = s_runner(cJSON_IsString(name) ? name->valuestring : "", input, &is_error);
        cJSON_Delete(input);
        // The response is an object: the result as JSON when it is, else its text
        cJSON *result = output ? cJSON_Parse(output) : NULL;
        if (!cJSON_IsObject(result)) {
            cJSON_Delete(result);
            result = cJSON_CreateObject();
            cJSON_AddStringToObject(result, is_error || !output ? "error" : "result",
                                    output ? output : "tool unavailable");
        }
        free(output);
        cJSON *response_part = cJSON_CreateObject();
        cJSON *response = cJSON_AddObjectToObject(response_part, "functionResponse");
        if (cJSON_IsString(id)) {
            cJSON_AddStringToObject(response, "id", id->valuestring);
        }
        cJSON_AddStringToObject(response, "name", cJSON_IsString(name) ? name->valuestring : "");
        cJSON_AddItemToObject(response, "response", result);
        cJSON_AddItemToArray(responses, response_part);
    }
    cJSON_AddItemToArray(s_messages, user);
}

static void stream_free(stream_t *st)
{
    llm_sse_free(&st->sse);
    cJSON_Delete(st->parts);
    free(st->signature);
    free(st);
}

static llm_error_t gemini_stream(void *state, const char *text, llm_text_cb_t on_text, void *ctx, char **full_text)
{
    if (full_text) {
        *full_text = NULL;
    }
    if (!s_key || !s_messages) {
        return LLM_ERR_NO_KEY;
    }

    llm_answer_t answer;
    llm_answer_begin(&answer, on_text, ctx);
    llm_error_t error = LLM_OK;
    int answer_tokens = 0;
    int thoughts_tokens = 0;
    char finish[32] = "";
    bool blocked = false;
    char *signature = NULL;

    /* Append the question; the contents of this exchange are removed again if it fails */
    int start_count = cJSON_GetArraySize(s_messages);
    add_message("user", text, NULL);

    for (int round = 0; ; round++) {
        // In PSRAM: the caller's stack (the SR handler task) is short
        stream_t *st = heap_caps_calloc(1, sizeof(stream_t), MALLOC_CAP_SPIRAM);
        if (!st) {
            error = LLM_ERR_OTHER;
            break;
        }
        st->answer = &answer;
        st->sse.on_event = sse_dispatch;
        st->sse.ctx = st;
        st->error = LLM_OK;
        st->parts = cJSON_CreateArray();
        stream_once(st);
        if (st->retry && round == 0) {
            stream_free(st);
            round = -1;         // the same question, without Google Search
            continue;
        }
        error = st->error;
        strlcpy(finish, st->finish, sizeof(finish));
        blocked |= st->blocked;
        answer_tokens += st->output_tokens;
        thoughts_tokens += st->thoughts_tokens;
        llm_str_set(&signature, st->signature);
        if (error != LLM_OK || st->call_count == 0 || !s_runner) {
            stream_free(st);
            break;
        }
        if (round >= GEMINI_TOOL_ROUNDS_MAX) {
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
    }

    if (error == LLM_OK) {
        bool safety = strcmp(finish, "SAFETY") == 0 || strcmp(finish, "PROHIBITED_CONTENT") == 0 ||
                      strcmp(finish, "BLOCKLIST") == 0 || strcmp(finish, "SPII") == 0;
        if ((blocked || safety) && answer.text.len == 0) {
            ESP_LOGW(TAG, "Request declined (%s)", blocked ? "prompt blocked" : finish);
            error = LLM_ERR_REFUSAL;
        } else if (strcmp(finish, "MAX_TOKENS") == 0) {
            ESP_LOGW(TAG, "Answer truncated (MAX_TOKENS)");
        }
        if (error == LLM_OK && answer.text.len == 0) {
            ESP_LOGE(TAG, "No text in answer (finish: %s)", finish[0] ? finish : "-");
            error = LLM_ERR_OTHER;
        }
    }
    ESP_LOGI(TAG, "first text after %d ms, total %d ms, finish: %s", llm_answer_first_ms(&answer),
             llm_answer_total_ms(&answer), finish[0] ? finish : "-");
    llm_answer_stats(&answer, answer_tokens, thoughts_tokens);

    // The history keeps the question and the final answer: the calls and their results (used already) would be
    // sent again with every later question
    llm_history_drop_from(s_messages, start_count + (error == LLM_OK));
    if (error == LLM_OK) {
        add_message("model", answer.text.p, signature);
        llm_history_trim(s_messages, NULL);
        if (full_text) {
            *full_text = answer.text.p;
            answer.text.p = NULL;
        }
    }
    llm_buf_free(&answer.text);
    free(signature);
    return error;
}

static void gemini_set_tools(void *state, const cJSON *definitions, llm_tool_runner_t runner)
{
    s_tools = definitions;
    s_runner = definitions ? runner : NULL;
}

const llm_backend_t llm_backend_gemini = {
    .init = gemini_init,
    .set_instructions = gemini_set_instructions,
    .reset = gemini_reset,
    .history = gemini_history,
    .set_history = gemini_set_history,
    .prewarm = gemini_prewarm,
    .stream = gemini_stream,
    .set_tools = gemini_set_tools,
};
