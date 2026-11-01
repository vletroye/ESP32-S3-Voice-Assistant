/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * The providers, in one table: see ai_providers.h.
 */

#include <stddef.h>
#include <string.h>
#include "settings.h"
#include "stt_stream.h"
#include "tts_stream.h"
#include "ai_provider_anthropic.h"
#include "ai_provider_openai.h"
#include "ai_provider_google.h"
#include "ai_provider_openrouter.h"
#include "ai_provider_xai.h"
#include "ai_provider_mistral.h"
#include "ai_provider_perplexity.h"
#include "ai_providers.h"

/* The field of sys_param_t holding each provider's key, in the order of s_providers */
static const size_t s_key_offsets[] = {
    offsetof(sys_param_t, claude_key),
    offsetof(sys_param_t, key),
    offsetof(sys_param_t, gemini_key),
    offsetof(sys_param_t, openrouter_key),
    offsetof(sys_param_t, xai_key),
    offsetof(sys_param_t, mistral_key),
    offsetof(sys_param_t, perplexity_key),
    offsetof(sys_param_t, deepgram_key),
    offsetof(sys_param_t, groq_key),
    offsetof(sys_param_t, elevenlabs_key),
};

/*
 * The chat providers first, in the order of llm_provider_t. The effort of the reasoning mode: "high" for all of them
 * (Claude's output_config.effort, OpenAI's reasoning.effort, Gemini's thinkingLevel, the reasoning_effort of the
 * others)
 */
static const ai_provider_t s_providers[] = {
    {
        "anthropic", "Anthropic", "Claude", "Claude_key", LLM_PROVIDER_CLAUDE, "high", true, &llm_backend_claude, NULL, NULL,
        { "anthropic", "anthropique", "anthropik", "claude", "clode", NULL },
    },
    {
        "openai", "OpenAI", "GPT", "ChatGPT_key", LLM_PROVIDER_OPENAI, "high", true, &llm_backend_openai, &stt_backend_openai,
        &tts_backend_openai, { "openai", "open ai", "open a i", "openaï", "gpt", "chatgpt", "chat gpt" },
    },
    {
        "google", "Google", "Gemini", "Gemini_key", LLM_PROVIDER_GEMINI, "high", true, &llm_backend_gemini, NULL, NULL,
        { "google", "gougle", "gemini", "gémini", "jemini", NULL },
    },
    {
        "openrouter", "OpenRouter", "OpenRouter", "OpenRouter_key", LLM_PROVIDER_OPENROUTER, "high", false,
        &llm_backend_openrouter, NULL, &tts_backend_openrouter,
        { "openrouter", "open router", "open routeur", "openrouteur", "open rooter", NULL },
    },
    {
        "xai", "xAI", "Grok", "XAI_key", LLM_PROVIDER_XAI, "high", true, &llm_backend_xai, NULL, NULL,
        { "xai", "x ai", "ix ai", "grok", "grock", "groc", "groque" },
    },
    {
        "mistral", "Mistral AI", "Mistral", "Mistral_key", LLM_PROVIDER_MISTRAL, "high", true, &llm_backend_mistral,
        &stt_backend_mistral, &tts_backend_mistral, { "mistral", "mistrale", "mistral ai", NULL },
    },
    {
        "perplexity", "Perplexity", "Perplexity", "Perplexity_key", LLM_PROVIDER_PERPLEXITY, "high", true,
        &llm_backend_perplexity, NULL, NULL,
        { "perplexity", "perplexité", "perplexiti", "perplexite", "sonar", "sonare", NULL },
    },
    // Speech services only
    {
        "deepgram", "Deepgram", "Deepgram", "Deepgram_key", LLM_PROVIDER_COUNT, NULL, false, NULL, &stt_backend_deepgram,
        &tts_backend_deepgram, { "deepgram", "deep gram", NULL },
    },
    {
        "groq", "Groq", "Groq", "Groq_key", LLM_PROVIDER_COUNT, NULL, false, NULL, &stt_backend_groq, NULL, { "groq", NULL },
    },
    {
        "elevenlabs", "ElevenLabs", "ElevenLabs", "ElevenLabs_key", LLM_PROVIDER_COUNT, NULL, false, NULL,
        &stt_backend_elevenlabs, &tts_backend_elevenlabs, { "elevenlabs", "eleven labs", NULL },
    },
};

_Static_assert(sizeof(s_providers) / sizeof(s_providers[0]) == sizeof(s_key_offsets) / sizeof(s_key_offsets[0]),
               "a key for each provider");

size_t ai_provider_count(void)
{
    return sizeof(s_providers) / sizeof(s_providers[0]);
}

const ai_provider_t *ai_provider_at(size_t i)
{
    return i < ai_provider_count() ? &s_providers[i] : NULL;
}

const ai_provider_t *ai_provider_find(const char *id)
{
    for (size_t i = 0; id && i < ai_provider_count(); i++) {
        if (strcmp(id, s_providers[i].id) == 0) {
            return &s_providers[i];
        }
    }
    return NULL;
}

const ai_provider_t *ai_provider_of_llm(llm_provider_t llm)
{
    return llm < LLM_PROVIDER_COUNT ? &s_providers[llm] : NULL;
}

const char *ai_provider_key(const ai_provider_t *p)
{
    if (!p) {
        return "";
    }
    const char *key = (const char *)settings_get_parameter() + s_key_offsets[p - s_providers];
    return key;
}
