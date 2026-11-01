/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Client of the Responses APIs (OpenAI, xAI), shared by their ai_provider_<name>.c: server-sent events, stateless
 * (store: false, the conversation kept on the device), the provider's web search (the model decides when to search;
 * a model refusing it is asked again without, until another model) and the device's tools as functions (their
 * output items, encrypted reasoning included, given back with the results). Each provider gives what differs
 * (rc_config_t) and keeps its own state (rc_client_t).
 */

#pragma once

#include "llm_backend.h"
#include "llm_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The shape of the web search tool */
typedef enum {
    RC_SEARCH_OPENAI,       /* {"type": "web_search", "search_context_size": "low", "user_location": {...}} */
    RC_SEARCH_PLAIN,        /* {"type": "web_search"} (xAI) */
} rc_search_t;

typedef struct {
    const char *tag;                /* of the logs */
    const char *base_url;           /* ".../v1/" ("responses" and "models/<model>" follow), unless set by the client */
    rc_search_t search;
    const char *web_search_cost;    /* "openai:web_search": its key in [per_call] of AGENTS.INI */
    bool cache_key;                 /* "prompt_cache_key": the conversation id, routing to its prompt cache */
} rc_config_t;

/* A provider's state: static, { .config = &its_config }, the rest made by rc_init */
typedef struct {
    const rc_config_t *config;
    char *base_url;                 /* set by the provider (OpenAI_url of CONFIG.INI), else the config's */
    char url[160];
    char prewarm_url[160];
    llm_http_t http;
    char *auth;                     /* "Bearer <key>" */
    char *model;
    char *instructions;
    char *effort;
    char conv_id[LLM_CONV_ID_LEN];  /* new with each conversation (config's cache_key) */
    bool web_refused;               /* the model refused the web search: asked without it, until another model */
    bool encrypted_refused;         /* the model refused to give its reasoning items */
    bool api_refused;               /* the model refused the Responses API itself (404...): the provider decides */
    bool tools_off;                 /* the last round of a question: no more calls (tool_choice none), an answer is due */
    /* The device's tools (definitions of tools.c) and the function running them, NULL for none */
    const cJSON *tools;
    llm_tool_runner_t runner;
    /* Conversation history, sent with each request: [{role, content}] at rest */
    cJSON *messages;
} rc_client_t;

esp_err_t rc_init(void *client, const char *api_key, const char *model, const char *instructions, const char *effort);
void rc_set_instructions(void *client, const char *instructions);
void rc_reset(void *client);
const cJSON *rc_history(void *client);
void rc_set_history(void *client, cJSON *messages);
void rc_prewarm(void *client);
llm_error_t rc_stream(void *client, const char *text, llm_text_cb_t on_text, void *ctx, char **full_text);
void rc_set_tools(void *client, const cJSON *definitions, llm_tool_runner_t runner);

/* The table of a Responses provider: RC_BACKEND(s_client) */
#define RC_BACKEND(client) {                                                                            \
        .state = &(client), .init = rc_init, .set_instructions = rc_set_instructions,                    \
        .reset = rc_reset, .history = rc_history, .set_history = rc_set_history, .prewarm = rc_prewarm,  \
        .stream = rc_stream, .set_tools = rc_set_tools,                                                  \
    }

#ifdef __cplusplus
}
#endif
