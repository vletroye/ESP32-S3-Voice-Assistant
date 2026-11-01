/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * xAI client (Grok models): Chat Completions API (OpenAI-compatible, ai_provider_chat_completions.c).
 */

#include "ai_provider_xai.h"
#include "ai_provider_chat_completions.h"

/* A team without credit gets a 403 that says so */
static const char *const s_credit_words[] = { "spending limit", NULL };

static const cc_config_t s_config = {
    .tag = "xai",
    .url = "https://api.x.ai/v1/chat/completions",
    // A tiny free answer (the key's details): opens the TLS connection in advance
    .prewarm_url = "https://api.x.ai/v1/api-key",
    .reasoning = CC_REASONING_EFFORT,
    .usage = CC_USAGE_STREAM_OPTIONS,
    .credit_words = s_credit_words,
    // Function calling: the device's tools (weather, radio, volume, Homey)
    .tools = true,
};

static cc_client_t s_client = { .config = &s_config };

const llm_backend_t llm_backend_xai = CC_BACKEND(s_client);
