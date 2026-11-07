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
    short order;            /* its place among the models of AGENTS.INI (0 first), -1 for a free OpenRouter one */
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
    const char *provider;   /* "openai", "mistral", "deepgram" */
    const char *spoken;     /* said in the spoken list, and to choose it */
} llm_speech_model_t;

/** How many models of this kind */
size_t llm_speech_count(llm_speech_kind_t kind);

/**
 * Models disabled in AGENTS.INI by a "-" before their id ("-mistral-large-4 = ..."): offered nowhere (Settings,
 * voice, model test), only shown unchecked on the lists of the Settings screen
 */
size_t llm_disabled_count(void);
const llm_model_t *llm_disabled_model(size_t index);

/** Every speech model of this kind, enabled or not (file order), NULL past the end; *enabled tells which */
size_t llm_speech_all_count(llm_speech_kind_t kind);
const llm_speech_model_t *llm_speech_all(llm_speech_kind_t kind, size_t index, bool *enabled);

/**
 * @brief Enable or disable a chat model of AGENTS.INI: the "-" of its line added or removed, and the lists in
 *        memory changed at once (no restart). The model in use is not to be disabled (the caller checks).
 * @return false if not found, or the file could not be written
 */
bool llm_set_model_enabled(const char *id, bool enabled);

/** The same for a transcription or speech model of [stt] / [tts] */
bool llm_set_speech_enabled(llm_speech_kind_t kind, const char *id, bool enabled);

/** The model of this kind at this index (file order, the enabled ones), NULL past the end */
const llm_speech_model_t *llm_speech_model(llm_speech_kind_t kind, size_t index);

/**
 * @brief Whose voices of [voices] a speech model takes: its provider ("openai", "mistral", "deepgram"), or for a
 *        model reached through OpenRouter its own id ("deepgram/aura-2"), each having its own voices
 */
const char *llm_speech_voice_owner(const llm_speech_model_t *model);

/** The model of this kind with this id, NULL if not listed */
const llm_speech_model_t *llm_speech_find(llm_speech_kind_t kind, const char *id);

/* Voices of AGENTS.INI ([voices]): those of the speech provider in use that speak the language are offered */
typedef struct llm_voice {
    const char *name;       /* unique, said and shown ("Lea"); kept in CONFIG.INI (Voice) */
    const char *provider;   /* "openai", "mistral", "deepgram" */
    const char *id;         /* voice of the provider's API ("marin", "voice_589") */
    const char *languages;  /* "fr,en", "*" for every one */
    const char *emotion;    /* "calm", NULL for none */
    const char *pace;       /* "slow", NULL for none */
    char gender;            /* 'f' a woman's voice, 'm' a man's, 0 neither or not known */
} llm_voice_t;

/** How many voices in [voices] */
size_t llm_voice_count(void);

/** The voice at this index (file order), NULL past the end */
const llm_voice_t *llm_voice(size_t index);

/** True when the voice is of this provider and speaks this language (2-letter code) */
bool llm_voice_offered(const llm_voice_t *voice, const char *provider, const char *lang);

/**
 * @brief Listed to choose from (Settings screen, "change de voix"): the voices of the provider that speak the
 *        language; when none does, those that speak English; when none does either, its first voice
 */
bool llm_voice_listed(const llm_voice_t *voice, const char *provider, const char *lang);

/**
 * @brief The voice to speak with: the one named (case ignored) if it is listed (llm_voice_listed), else the first
 *        one listed; NULL if the provider has none
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
 * Free OpenRouter model never offered: a "-" before its line of [openrouter_free] in AGENTS.INI, or an id of the
 * [openrouter_free_exclude] of an older file (its id, or an id ending with '*' that it starts with: "nvidia/"
 * followed by '*')
 */
bool llm_free_excluded(const char *id);

/**
 * @brief Never offer a free OpenRouter model: a "-" before its line of [openrouter_free] in AGENTS.INI (the line
 *        added if missing). Offered models are not changed (openrouter_free_init() does it).
 */
void llm_free_exclude_add(const char *id, const char *comment);

/** Free OpenRouter models found in its live list: those [openrouter_free] does not list yet added to it */
void llm_free_found(const char *const *ids, const char *const *names, int n);

/** The lines of [openrouter_free] (file order): its id, its name, enabled (no "-"); NULL past the end */
size_t llm_free_count(void);
const char *llm_free_line(size_t index, const char **name, bool *enabled);

/** A "-" before the line of a free OpenRouter model, or removed (openrouter_free_init() then offers again) */
bool llm_set_free_enabled(const char *id, bool enabled);

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
 * @brief A question to the model in use, apart from the conversation: with these instructions, an empty history and
 *        no tools; the conversation, its instructions and its tools given back after. Blocking (until the end of the
 *        answer). The games between two devices (game.c).
 * @param answer The whole answer (allocated, to be freed), NULL on error
 */
llm_error_t llm_ask_apart(const char *system_prompt, const char *question, char **answer);

/**
 * Provider serving a model id: its section of AGENTS.INI; for a model not listed, "vendor/model" is OpenRouter,
 * "gpt-..." and "o<digit>..." are OpenAI, "gemini-..." Google, "grok-..." xAI, "mistral-...", "magistral-...",
 * "ministral-..." Mistral AI, "sonar..." Perplexity, anything else Claude
 */
llm_provider_t llm_provider_of(const char *model);

/** The name of the provider's chat models ("Claude", "GPT", "Gemini", "OpenRouter", "Grok"...), ai_providers.c */
const char *llm_provider_name(llm_provider_t provider);

/**
 * @brief Configure the client of the model's provider, with its key of the settings (ai_providers.c). The
 *        strings are copied.
 *
 * @param model           Model id, e.g. "claude-haiku-5-5" or "gpt-6-luna"
 * @param openai_base_url OpenAI base URL ending with '/', e.g. "https://api.openai.com/v1/"
 * @param system_prompt   Instructions sent with every request
 */
esp_err_t llm_init(const char *model, const char *openai_base_url, const char *system_prompt);

/** Replace the instructions (e.g. after a note was remembered); the conversation is kept */
void llm_set_system_prompt(const char *system_prompt);

/** Web searches made for the current (or last) answer */
int llm_web_searches(void);

/**
 * The tools changed (Homey's added): given to the model in use from now on, between two answers (the list of its
 * provider: tools_definitions(), with web_search for a provider without its own search)
 */
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

/** Forget the conversation history, also the saved one; the reasoning mode ends */
void llm_reset(void);

/*
 * The reasoning mode ("j'ai une question difficile"): the model of the conversation reasons more (the effort of its
 * provider in ai_providers.c), until a new conversation ("au revoir"), another model or a restart
 */
bool llm_reasoning(void);

/** The model in use can reason (its effort in AGENTS.INI is not "-") */
bool llm_reasoning_possible(void);

/** Turned on or off, the conversation kept; false if the model cannot reason */
bool llm_set_reasoning(bool on);

/** esp_timer time of the last question answered (0 after a new conversation) */
int64_t llm_last_exchange_us(void);

/** The pause in the conversation dealt with (asked whether to go on): as if its last exchange were now */
void llm_exchange_touch(void);

/* Tokens an answer may take, its reasoning included: a short spoken answer, or a long reasoning */
#define LLM_MAX_OUTPUT_TOKENS           2048
#define LLM_MAX_OUTPUT_TOKENS_REASONING 8192

/** The tokens the clients ask for: LLM_MAX_OUTPUT_TOKENS, or ..._REASONING in the reasoning mode */
int llm_max_output_tokens(void);

/** The time the clients wait for the answer to go on: longer in the reasoning mode (ms) */
int llm_answer_timeout_ms(void);

/**
 * @brief Save the conversation history to flash if it changed since the last save.
 *
 * Restored by llm_init() when the model is the same, so that a conversation can go on after the device
 * slept or restarted. Writing to flash pauses the other tasks briefly: call it when no audio is playing,
 * from a task whose stack is in internal RAM.
 */
void llm_history_save(void);

/** True when the conversation has changes not saved yet */
bool llm_history_unsaved(void);

#ifdef __cplusplus
}
#endif
