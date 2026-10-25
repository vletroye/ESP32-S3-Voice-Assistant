/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"
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
 * @brief Forget the conversation history.
 */
void claude_client_reset(void);

#ifdef __cplusplus
}
#endif
