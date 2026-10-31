/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Client of the OpenAI-compatible Chat Completions APIs (OpenRouter, xAI, Mistral AI), shared by their
 * ai_provider_<name>.c: server-sent events, the conversation kept on the device and sent with each request. Each
 * provider gives what differs (cc_config_t) and keeps its own state (cc_client_t).
 */

#pragma once

#include "llm_backend.h"
#include "llm_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How the reasoning effort is sent */
typedef enum {
    CC_REASONING_EFFORT,        /* "reasoning_effort": "low" (xAI, Mistral) */
    CC_REASONING_OBJECT,        /* "reasoning": {"effort": "low", "exclude": true} (OpenRouter) */
} cc_reasoning_t;

/* How the usage (tokens) is asked for */
typedef enum {
    CC_USAGE_LAST_CHUNK,        /* not asked: it comes with the last chunk (Mistral, which rejects unknown fields) */
    CC_USAGE_INCLUDE,           /* "usage": {"include": true} (OpenRouter) */
    CC_USAGE_STREAM_OPTIONS,    /* "stream_options": {"include_usage": true} (xAI) */
} cc_usage_t;

typedef struct {
    const char *tag;                /* of the logs */
    const char *url;                /* ".../v1/chat/completions" */
    const char *prewarm_url;        /* a free GET with a small answer; a "%s" in it: the model */
    const char *const *headers;     /* more headers: name, value, ..., NULL; NULL for none */
    cc_reasoning_t reasoning;
    cc_usage_t usage;
    /* Words of an error message meaning no credit, besides "credit": "billing", "spending limit"... NULL-ended */
    const char *const *credit_words;
} cc_config_t;

/* A provider's state: static, { .config = &its_config }, the rest made by cc_init */
typedef struct {
    const cc_config_t *config;
    llm_http_t http;
    char *auth;                 /* "Bearer <key>" */
    char *model;
    char *instructions;
    char *effort;
    char prewarm_url[160];
    /* Conversation history: [{role: "user" | "assistant", content}] (the instructions are added per request) */
    cJSON *messages;
} cc_client_t;

esp_err_t cc_init(void *client, const char *api_key, const char *model, const char *instructions, const char *effort);
void cc_set_instructions(void *client, const char *instructions);
void cc_reset(void *client);
const cJSON *cc_history(void *client);
void cc_set_history(void *client, cJSON *messages);
void cc_prewarm(void *client);
llm_error_t cc_stream(void *client, const char *text, llm_text_cb_t on_text, void *ctx, char **full_text);

/* The table of a Chat Completions provider: CC_BACKEND("XAI_key", s_client) */
#define CC_BACKEND(key, client) {                                                                       \
        .key_name = (key), .state = &(client), .init = cc_init, .set_instructions = cc_set_instructions, \
        .reset = cc_reset, .history = cc_history, .set_history = cc_set_history, .prewarm = cc_prewarm,  \
        .stream = cc_stream,                                                                             \
    }

#ifdef __cplusplus
}
#endif
