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
 * @brief Configure the Gemini client (Gemini API, streamGenerateContent). The strings are copied.
 *
 * @param api_key         Gemini API key (Google AI Studio)
 * @param model           Model id, e.g. "gemini-3.8-flash"
 * @param instructions    System instructions sent with every request
 * @param thinking_level  thinkingConfig.thinkingLevel ("minimal", "low", ...), NULL or "" for the model default
 */
esp_err_t gemini_chat_init(const char *api_key, const char *model, const char *instructions,
                           const char *thinking_level);

/**
 * @brief Ask a question and stream the answer, keeping a short conversation history.
 *
 * @param full_text On success, the complete answer (to be freed with free()), NULL otherwise
 */
llm_error_t gemini_chat_stream(const char *text, llm_text_cb_t on_text, void *ctx, char **full_text);

/** Open the HTTPS connection in advance (blocking) */
void gemini_chat_prewarm(void);

/** Forget the conversation history */
void gemini_chat_reset(void);

/** Replace the instructions (copied) */
void gemini_chat_set_instructions(const char *instructions);

/** Conversation history (JSON array of contents, owned by the client), to save it */
const cJSON *gemini_chat_history(void);

/** Replace the conversation history (taken over by the client), e.g. when restored at start-up */
void gemini_chat_set_history(cJSON *messages);

#ifdef __cplusplus
}
#endif
