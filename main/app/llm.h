/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Chat model answering the questions: Claude (Anthropic), GPT (OpenAI), Gemini (Google) or a model reached
 * through OpenRouter, chosen by the model id.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LLM_PROVIDER_CLAUDE = 0,
    LLM_PROVIDER_OPENAI,
    LLM_PROVIDER_GEMINI,
    LLM_PROVIDER_OPENROUTER,
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

/** Models offered on the Settings screen (AGENTS.INI) */
typedef struct {
    const char *id;         /* API model id */
    const char *label;      /* Name shown in the list */
    const char *effort;     /* Claude output_config.effort / OpenAI reasoning.effort / Gemini thinkingLevel /
                               OpenRouter reasoning.effort, NULL for the default */
    const char *spoken;     /* Short name in the spoken list of models ("Haiku") */
    const char *thinking;   /* Claude thinking.type ("disabled", "between_tools"), "default" to leave it out,
                               NULL to choose it from the model's name */
    llm_provider_t provider;
    const char *speed;      /* "fast", "average" or "slow", from the last model test (model_test.c), NULL if none */
} llm_model_t;

/** Measures of the last answer, set by the client of its provider */
typedef struct {
    int first_ms;           /* until the first words of the answer, -1 if none */
    int total_ms;           /* until the end of the answer */
    int answer_tokens;      /* tokens of the answer, without the reasoning */
    int reasoning_tokens;   /* reasoning tokens, -1 when not given apart */
} llm_stats_t;
extern llm_stats_t llm_last_stats;

/* The models of AGENTS.INI (llm_load_models()), then the free OpenRouter ones (openrouter_free.c) */
#define LLM_FILE_MODELS_MAX         32
#define LLM_OPENROUTER_FREE_MAX     16      /* Free_models (at most 15) and "openrouter/free" */
extern llm_model_t *llm_models;      /* PSRAM, allocated by llm_load_models() */
extern size_t llm_model_count;

/**
 * @brief Read the models offered from AGENTS.INI (/spiffs/agents.ini, installed from the firmware's version when
 *        missing or emptied). At start-up, after SPIFFS is mounted and before the Settings screen is built, from a
 *        task whose stack is in internal RAM.
 */
void llm_load_models(void);

/**
 * @brief Replace the free OpenRouter models offered, put after the models of AGENTS.INI. The strings are copied.
 */
void llm_set_openrouter_free_models(const llm_model_t *models, size_t count);

/**
 * Free OpenRouter model excluded by the [openrouter_free_exclude] section of AGENTS.INI: its id, or an id
 * ending with '*' that it starts with ("nvidia/" followed by '*')
 */
bool llm_free_excluded(const char *id);

/**
 * @brief Exclude a free OpenRouter model: added under [openrouter_free_exclude] of AGENTS.INI (the section is
 *        created if missing), with a comment. Offered models are not changed (openrouter_free_init() does it).
 */
void llm_free_exclude_add(const char *id, const char *comment);

/** Number of models of AGENTS.INI (the first ones of llm_models) */
size_t llm_agents_count(void);

/**
 * @brief Write the speed of the models of AGENTS.INI into the file: speeds[i] for llm_models[i] (NULL: unchanged),
 *        put after the spoken name. Comments and the other lines are kept.
 */
void llm_save_speeds(const char *const *speeds);

/**
 * @brief Model test: ask one question to a model without the conversation nor tools, with the same instructions.
 *        The configured model and its conversation are kept. Blocking (until the end of the answer).
 * @param stats Its measures (first_ms -1 when it did not answer)
 */
llm_error_t llm_probe(const llm_model_t *model, const char *question, llm_stats_t *stats);

/**
 * Provider serving a model id: its section of AGENTS.INI; for a model not listed, "vendor/model" is OpenRouter,
 * "gpt-..." and "o<digit>..." are OpenAI, "gemini-..." Google, anything else Claude
 */
llm_provider_t llm_provider_of(const char *model);

/** Provider name as said to the user ("Claude", "GPT", "Gemini", "OpenRouter") */
const char *llm_provider_name(llm_provider_t provider);

/**
 * @brief Configure the client of the model's provider. The strings are copied.
 *
 * @param model           Model id, e.g. "claude-haiku-5-5" or "gpt-6-luna"
 * @param claude_key      Anthropic key (may be empty when a GPT model is used)
 * @param openai_key      OpenAI key
 * @param openai_base_url OpenAI base URL ending with '/', e.g. "https://api.openai.com/v1/"
 * @param gemini_key      Gemini key (may be empty when no Gemini model is used)
 * @param openrouter_key  OpenRouter key (may be empty when no OpenRouter model is used)
 * @param system_prompt   Instructions sent with every request
 */
esp_err_t llm_init(const char *model, const char *claude_key, const char *openai_key, const char *openai_base_url,
                   const char *gemini_key, const char *openrouter_key, const char *system_prompt);

/** Replace the instructions (e.g. after a note was remembered); the conversation is kept */
void llm_set_system_prompt(const char *system_prompt);

/** Web searches made for the current (or last) answer */
int llm_web_searches(void);

/** Tool definitions offered to Claude from now on (kept by the caller), swapped between two answers */
void llm_set_tools(const cJSON *definitions);

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
