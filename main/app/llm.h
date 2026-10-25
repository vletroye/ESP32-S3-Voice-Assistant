/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Chat model answering the questions: Claude (Anthropic) or GPT (OpenAI), chosen by the model id.
 */

#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LLM_PROVIDER_CLAUDE = 0,
    LLM_PROVIDER_OPENAI,
    LLM_PROVIDER_COUNT,
} llm_provider_t;

typedef enum {
    LLM_OK = 0,
    LLM_ERR_NO_KEY,         /* No key configured for the provider */
    LLM_ERR_NETWORK,        /* No response (Wi-Fi, DNS, TLS, timeout, stream cut) */
    LLM_ERR_AUTH,           /* Key rejected (401/403) */
    LLM_ERR_CREDIT,         /* Credit balance too low / quota exceeded */
    LLM_ERR_BUSY,           /* Rate limited or overloaded (429, 5xx) */
    LLM_ERR_REFUSAL,        /* Request declined by the model's safeguards */
    LLM_ERR_OTHER,          /* Any other error (invalid request, unexpected response) */
} llm_error_t;

/**
 * @brief Called from the streaming loop each time the model produces text.
 *
 * @param delta New text (NUL-terminated, only valid during the call), or NULL when the model pauses for a
 *              tool (what was said so far can be spoken meanwhile)
 * @param ctx   User context given to the stream function
 */
typedef void (*llm_text_cb_t)(const char *delta, void *ctx);

/** Models offered on the Settings screen */
typedef struct {
    const char *id;         /* API model id */
    const char *label;      /* Name shown in the list */
    const char *effort;     /* Claude output_config.effort / OpenAI reasoning.effort, NULL for the default */
    const char *spoken;     /* Short name in the spoken list of models ("Haiku") */
} llm_model_t;

extern const llm_model_t llm_models[];
extern const size_t llm_model_count;

/** Provider serving a model id ("gpt-..." and "o<digit>..." are OpenAI, anything else Claude) */
llm_provider_t llm_provider_of(const char *model);

/** Provider name as said to the user ("Claude", "GPT") */
const char *llm_provider_name(llm_provider_t provider);

/**
 * @brief Configure the client of the model's provider. The strings are copied.
 *
 * @param model           Model id, e.g. "claude-haiku-5-5" or "gpt-6-luna"
 * @param claude_key      Anthropic key (may be empty when a GPT model is used)
 * @param openai_key      OpenAI key
 * @param openai_base_url OpenAI base URL ending with '/', e.g. "https://api.openai.com/v1/"
 * @param system_prompt   Instructions sent with every request
 */
esp_err_t llm_init(const char *model, const char *claude_key, const char *openai_key, const char *openai_base_url,
                   const char *system_prompt);

/** Replace the instructions (e.g. after a note was remembered); the conversation is kept */
void llm_set_system_prompt(const char *system_prompt);

/** Web searches made for the current (or last) answer */
int llm_web_searches(void);

/** Where the user lives (2-letter country code, town): localizes the web search (Claude) */
void llm_set_location(const char *country, const char *city);

/** Provider of the configured model */
llm_provider_t llm_provider(void);

/**
 * @brief Ask a question and stream the answer, keeping a short conversation history.
 *
 * @param full_text On success, the complete answer (to be freed with free()), NULL otherwise
 * @return LLM_OK, or the kind of error. Text may already have been streamed when an error occurs mid-answer.
 */
llm_error_t llm_stream(const char *text, llm_text_cb_t on_text, void *ctx, char **full_text);

/** Open the HTTPS connection in advance (blocking, ~1 s when not already open) */
void llm_prewarm(void);

/** Forget the conversation history, also the saved one */
void llm_reset(void);

/**
 * @brief Save the conversation history to flash if it changed since the last save.
 *
 * Restored by llm_init() when the model is the same, so that a conversation can go on after the device
 * slept or restarted. Writing to flash pauses the other tasks briefly: call it when no audio is playing,
 * from a task whose stack is in internal RAM.
 */
void llm_history_save(void);

#ifdef __cplusplus
}
#endif
