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
 * @brief Configure the GPT client (OpenAI Responses API, streaming). The strings are copied.
 *
 * @param base_url      OpenAI base URL ending with '/', e.g. "https://api.openai.com/v1/"
 * @param api_key       OpenAI key
 * @param model         Model id, e.g. "gpt-6-luna"
 * @param instructions  Instructions sent with every request
 * @param effort        reasoning.effort ("none", "low", ...), NULL or "" to use the model default
 */
esp_err_t openai_chat_init(const char *base_url, const char *api_key, const char *model, const char *instructions,
                           const char *effort);

/**
 * @brief Ask a question and stream the answer, keeping a short conversation history.
 *
 * @param full_text On success, the complete answer (to be freed with free()), NULL otherwise
 */
llm_error_t openai_chat_stream(const char *text, llm_text_cb_t on_text, void *ctx, char **full_text);

/** Open the HTTPS connection in advance (blocking) */
void openai_chat_prewarm(void);

/** Forget the conversation history */
void openai_chat_reset(void);

/** Replace the instructions (copied) */
void openai_chat_set_instructions(const char *instructions);

/** Conversation history (JSON array of messages, owned by the client), to save it */
const cJSON *openai_chat_history(void);

/** Replace the conversation history (taken over by the client), e.g. when restored at start-up */
void openai_chat_set_history(cJSON *messages);

#ifdef __cplusplus
}
#endif
