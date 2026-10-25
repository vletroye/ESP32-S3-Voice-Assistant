/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"
#include "llm.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure the Claude client (Anthropic Messages API, streaming).
 *
 * The strings are copied. Can be called again to change the configuration.
 *
 * @param api_key       Anthropic API key
 * @param model         Model id, e.g. "claude-haiku-5-5"
 * @param system_prompt System prompt sent with every request
 * @param effort        output_config.effort ("low", "medium", ...), NULL or "" to use the model default
 *
 * Thinking is turned off when the model allows it (Haiku: disabled, Sonnet: between tool calls only);
 * Opus always thinks, its thinking blocks are kept in the history as the API requires.
 */
esp_err_t claude_client_init(const char *api_key, const char *model, const char *system_prompt, const char *effort);

/**
 * @brief Ask a question and stream the answer, keeping a short conversation history.
 *
 * @param text      User message
 * @param on_text   Called for each text delta as it arrives (may be NULL)
 * @param ctx       Passed to on_text
 * @param full_text On success, the complete answer (to be freed with free()), NULL otherwise
 * @return LLM_OK, or the kind of error. Text may already have been streamed when an error occurs mid-answer.
 */
llm_error_t claude_client_stream(const char *text, llm_text_cb_t on_text, void *ctx, char **full_text);

/**
 * @brief Open the HTTPS connection in advance (blocking, ~1 s when not already open).
 *
 * Called while the user speaks, so that the question does not pay for the TLS handshake.
 */
void claude_client_prewarm(void);

/**
 * @brief Runs a tool called by Claude.
 *
 * @param name     Tool name
 * @param input    Arguments (JSON object)
 * @param is_error Set to true when the result describes an error
 * @return Result given back to Claude (allocated, freed by the caller), NULL if the tool is unknown
 */
typedef char *(*claude_tool_runner_t)(const char *name, const cJSON *input, bool *is_error);

/** Offer tools: definitions (JSON array, kept by the caller) and the function running them */
void claude_client_set_tools(const cJSON *definitions, claude_tool_runner_t runner);

/** Web searches started during the current (or last) answer */
int claude_client_web_searches(void);

/** Replace the system prompt (copied) */
void claude_client_set_system(const char *system_prompt);

/**
 * @brief Forget the conversation history.
 */
void claude_client_reset(void);

/** Conversation history (JSON array of messages, owned by the client), to save it */
const cJSON *claude_client_history(void);

/** Replace the conversation history (taken over by the client), e.g. when restored at start-up */
void claude_client_set_history(cJSON *messages);

#ifdef __cplusplus
}
#endif
