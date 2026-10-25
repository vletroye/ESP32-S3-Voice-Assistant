/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLAUDE_OK = 0,
    CLAUDE_ERR_NO_KEY,      /* No Claude key configured */
    CLAUDE_ERR_NETWORK,     /* No response (Wi-Fi, DNS, TLS, timeout, stream cut) */
    CLAUDE_ERR_AUTH,        /* Key rejected (401/403) */
    CLAUDE_ERR_CREDIT,      /* Credit balance too low */
    CLAUDE_ERR_BUSY,        /* Rate limited or overloaded (429, 5xx, overloaded_error) */
    CLAUDE_ERR_REFUSAL,     /* Request declined by Claude's safeguards */
    CLAUDE_ERR_OTHER,       /* Any other error (invalid request, unexpected response) */
} claude_error_t;

/**
 * @brief Called from the streaming loop each time Claude produces text.
 *
 * @param delta New text (NUL-terminated, only valid during the call)
 * @param ctx   User context given to claude_client_stream()
 */
typedef void (*claude_text_cb_t)(const char *delta, void *ctx);

/**
 * @brief Configure the Claude client (Anthropic Messages API, streaming).
 *
 * The strings are copied. Can be called again to change the configuration.
 *
 * @param api_key       Anthropic API key
 * @param model         Model id, e.g. "claude-haiku-5-5"
 * @param system_prompt System prompt sent with every request
 * @param effort        output_config.effort ("low", "medium", ...), NULL or "" to use the model default
 */
esp_err_t claude_client_init(const char *api_key, const char *model, const char *system_prompt, const char *effort);

/**
 * @brief Ask a question and stream the answer, keeping a short conversation history.
 *
 * @param text      User message
 * @param on_text   Called for each text delta as it arrives (may be NULL)
 * @param ctx       Passed to on_text
 * @param full_text On success, the complete answer (to be freed with free()), NULL otherwise
 * @return CLAUDE_OK, or the kind of error. Text may already have been streamed when an error occurs mid-answer.
 */
claude_error_t claude_client_stream(const char *text, claude_text_cb_t on_text, void *ctx, char **full_text);

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
