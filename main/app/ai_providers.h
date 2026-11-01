/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * The providers, in one table: their id (AGENTS.INI), their names (said, shown, in the logs), their key
 * (CONFIG.INI), their chat client and their transcription and speech services, and how the transcription may
 * write them. Adding a provider: a line in ai_providers.c, and its clients.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "llm.h"

#ifdef __cplusplus
extern "C" {
#endif

struct stt_backend;
struct tts_backend;
struct llm_backend;

#define AI_PROVIDER_HEARD_MAX   7

typedef struct {
    const char *id;             /* "anthropic": section of AGENTS.INI, provider column of [stt] [tts] [voices] */
    const char *company;        /* "Anthropic": said and shown ("Quel fournisseur...", "ElevenLabs refuse...") */
    const char *family;         /* "Claude": the name of its chat models (logs) */
    const char *key_name;       /* "Claude_key" of CONFIG.INI */
    llm_provider_t llm;         /* its chat models, LLM_PROVIDER_COUNT for a speech service only */
    const char *reasoning_effort;   /* the effort of the reasoning mode ("j'ai une question difficile"), NULL if none */
    bool own_web_search;        /* its models search the web themselves (else the web_search tool, by Perplexity) */
    const struct llm_backend *chat;         /* its chat client, NULL if none */
    const struct stt_backend *stt;          /* its transcription service, NULL if none */
    const struct tts_backend *tts;          /* its speech service, NULL if none */
    const char *heard[AI_PROVIDER_HEARD_MAX];   /* as the transcription may write it (normalized), NULL-ended */
} ai_provider_t;

/** The providers: ai_provider_at(0 .. ai_provider_count() - 1) */
size_t ai_provider_count(void);
const ai_provider_t *ai_provider_at(size_t i);

/** By its id ("elevenlabs"), NULL if unknown */
const ai_provider_t *ai_provider_find(const char *id);

/** Of a kind of chat models (LLM_PROVIDER_CLAUDE...); never NULL for one below LLM_PROVIDER_COUNT */
const ai_provider_t *ai_provider_of_llm(llm_provider_t llm);

/** Its key, from the settings ("" if none) */
const char *ai_provider_key(const ai_provider_t *p);

#ifdef __cplusplus
}
#endif
