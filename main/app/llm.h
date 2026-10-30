/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Chat model answering the questions: Claude (Anthropic), GPT (OpenAI), Gemini (Google), Grok (xAI), Mistral
 * (Mistral AI), Sonar (Perplexity) or a model reached through OpenRouter, chosen by the model id.
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
    LLM_PROVIDER_XAI,
    LLM_PROVIDER_MISTRAL,
    LLM_PROVIDER_PERPLEXITY,
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
    LLM_ERR_EFFORT,         /* The reasoning effort of AGENTS.INI refused by the model: llm_effort_refused,
                               llm_effort_values (one text for every provider, not in llm_errors) */
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
                               OpenRouter reasoning.effort / xAI reasoning_effort ("-": not sent), NULL for the
                               default */
    const char *spoken;     /* Short name in the spoken list of models ("Haiku") */
    const char *thinking;   /* Claude thinking.type ("disabled", "between_tools"), "default" to leave it out,
                               NULL to choose it from the model's name */
    llm_provider_t provider;
    const char *speed;      /* "fast", "average" or "slow", from the last model test (model_test.c), NULL if none */
    double price_in;        /* per million tokens, in the currency of CONFIG.INI (AGENTS.INI) */
    double price_out;
    bool priced;            /* prices given (otherwise its cost is not counted) */
} llm_model_t;

/** Measures of the last answer, set by the client of its provider */
typedef struct {
    int first_ms;           /* until the first words of the answer, -1 if none */
    int total_ms;           /* until the end of the answer */
    int answer_tokens;      /* tokens of the answer, without the reasoning */
    int reasoning_tokens;   /* reasoning tokens, -1 when not given apart */
} llm_stats_t;
extern llm_stats_t llm_last_stats;

/* The models of AGENTS.INI (llm_load_models()), then the free OpenRouter ones (ai_provider_openrouter_free.c) */
#define LLM_FILE_MODELS_MAX         32
#define LLM_OPENROUTER_FREE_MAX     6       /* OpenRouter_free (at most 5) and "openrouter/free" */
extern llm_model_t *llm_models;      /* PSRAM, allocated by llm_load_models() */
extern size_t llm_model_count;

/**
 * @brief Read the models offered from AGENTS.INI (/spiffs/agents.ini, installed from the firmware's version when
 *        missing or emptied). At start-up, after SPIFFS is mounted and before the Settings screen is built, from a
 *        task whose stack is in internal RAM.
 */
void llm_load_models(void);

/* Transcription and speech models of AGENTS.INI ([stt], [tts]), chosen by voice */
typedef enum { LLM_SPEECH_STT, LLM_SPEECH_TTS } llm_speech_kind_t;

typedef struct {
    const char *id;         /* model id of the API ("gpt-live-transcribe", "voxtral-mini-tts-3-0") */
    const char *label;      /* shown name */
    const char *provider;   /* "openai", "mistral" */
    const char *spoken;     /* said in the spoken list, and to choose it */
} llm_speech_model_t;

/** How many models of this kind */
size_t llm_speech_count(llm_speech_kind_t kind);

/** The model of this kind at this index (file order), NULL past the end */
const llm_speech_model_t *llm_speech_model(llm_speech_kind_t kind, size_t index);

/** The model of this kind with this id, NULL if not listed */
const llm_speech_model_t *llm_speech_find(llm_speech_kind_t kind, const char *id);

/* Voices of AGENTS.INI ([voices]): those of the speech provider in use that speak the language are offered */
typedef struct llm_voice {
    const char *name;       /* unique, said and shown ("Lea"); kept in CONFIG.INI (Voice) */
    const char *provider;   /* "openai", "mistral" */
    const char *id;         /* voice of the provider's API ("marin", "voice_589") */
    const char *languages;  /* "fr,en", "*" for every one */
    const char *emotion;    /* "calm", NULL for none */
    const char *pace;       /* "slow", NULL for none */
} llm_voice_t;

/** How many voices in [voices] */
size_t llm_voice_count(void);

/** The voice at this index (file order), NULL past the end */
const llm_voice_t *llm_voice(size_t index);

/** True when the voice is of this provider and speaks this language (2-letter code) */
bool llm_voice_offered(const llm_voice_t *voice, const char *provider, const char *lang);

/**
 * @brief The voice to speak with: the one named (case ignored) if it is of the provider and speaks the language,
 *        else the first one that is, else the first one of the provider; NULL if the provider has none
 */
const llm_voice_t *llm_voice_for(const char *provider, const char *lang, const char *name);

/** LLM_ERR_EFFORT: the effort refused ("low") and the values the model takes ("high, none") */
extern char llm_effort_refused[16];
extern char llm_effort_values[64];

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

/**
 * @brief Prices per million tokens of a model (llm_models) or of a speech model ([speech] of AGENTS.INI), in the
 *        currency of CONFIG.INI
 * @return false when AGENTS.INI gives none
 */
bool llm_price_of(const char *id, double *price_in, double *price_out);

/** Price of a call ([per_call] of AGENTS.INI: "claude:web_search"); false when none is given */
bool llm_call_price_of(const char *id, double *price);

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
 * "gpt-..." and "o<digit>..." are OpenAI, "gemini-..." Google, "grok-..." xAI, "mistral-...", "magistral-...",
 * "ministral-..." Mistral AI, "sonar..." Perplexity, anything else Claude
 */
llm_provider_t llm_provider_of(const char *model);

/** Provider name as said to the user ("Claude", "GPT", "Gemini", "OpenRouter", "Grok", "Mistral", "Perplexity") */
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
 * @param xai_key         xAI key (may be empty when no Grok model is used)
 * @param mistral_key     Mistral AI key (may be empty when no Mistral model is used)
 * @param perplexity_key  Perplexity key (may be empty when no Perplexity model is used)
 * @param system_prompt   Instructions sent with every request
 */
esp_err_t llm_init(const char *model, const char *claude_key, const char *openai_key, const char *openai_base_url,
                   const char *gemini_key, const char *openrouter_key, const char *xai_key,
                   const char *mistral_key, const char *perplexity_key, const char *system_prompt);

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

/** The answer being streamed is dropped (double tap while "Je réfléchis"): the clients leave their stream at once,
 *  closing the connection; llm_stream() then returns an error, which the caller ignores. Cleared by
 *  llm_cancel_clear() (start of a listening session). */
void llm_cancel(void);
void llm_cancel_clear(void);
bool llm_cancelled(void);

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
