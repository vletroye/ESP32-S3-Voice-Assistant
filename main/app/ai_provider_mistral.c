/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Mistral AI client (Mistral Small, Medium, Large...): Chat Completions API (OpenAI-like,
 * ai_provider_chat_completions.c). Mistral rejects the fields it does not know: no stream_options (the usage
 * comes with the last chunk anyway). Its reasoning models give their answer as parts (thinking, text), and a
 * reasoning effort it refuses is reported with the values it takes.
 */

#include "ai_provider_mistral.h"
#include "ai_provider_chat_completions.h"

static const char *const s_credit_words[] = { "billing", "payment", "quota", NULL };

static const cc_config_t s_config = {
    .tag = "mistral",
    .url = "https://api.mistral.ai/v1/chat/completions",
    // A tiny free answer (the model's card): opens the TLS connection in advance
    .prewarm_url = "https://api.mistral.ai/v1/models/%s",
    .reasoning = CC_REASONING_EFFORT,
    .usage = CC_USAGE_LAST_CHUNK,
    .credit_words = s_credit_words,
    // Function calling: the device's tools (weather, radio, volume, Homey)
    .tools = true,
};

static cc_client_t s_client = { .config = &s_config };

const llm_backend_t llm_backend_mistral = CC_BACKEND(s_client);
