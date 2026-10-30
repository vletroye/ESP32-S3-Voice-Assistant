/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"
#include "cJSON.h"
#include "llm.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure the xAI client (Chat Completions API, streaming). The strings are copied.
 *
 * @param api_key       xAI key
 * @param model         Model id, e.g. "grok-4.7"
 * @param instructions  Instructions sent with every request (as the system message)
 * @param effort        reasoning_effort ("low", "medium", "high"), NULL, "" or "-" to leave it out (models that
 *                      do not take it)
 */
esp_err_t xai_chat_init(const char *api_key, const char *model, const char *instructions, const char *effort);

/**
 * @brief Ask a question and stream the answer, keeping a short conversation history.
 *
 * @param full_text On success, the complete answer (to be freed with free()), NULL otherwise
 */
llm_error_t xai_chat_stream(const char *text, llm_text_cb_t on_text, void *ctx, char **full_text);

/** Open the HTTPS connection in advance (blocking) */
void xai_chat_prewarm(void);

/** Forget the conversation history */
void xai_chat_reset(void);

/** Replace the instructions (copied) */
void xai_chat_set_instructions(const char *instructions);

/** Conversation history (JSON array of messages, owned by the client), to save it */
const cJSON *xai_chat_history(void);

/** Replace the conversation history (taken over by the client), e.g. when restored at start-up */
void xai_chat_set_history(cJSON *messages);

#ifdef __cplusplus
}
#endif
