/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * OpenRouter client: Chat Completions API (OpenAI-compatible, ai_provider_chat_completions.c). One key gives
 * access to the models of many providers.
 */

#include "ai_provider_openrouter.h"
#include "ai_provider_chat_completions.h"

/* Shown on openrouter.ai for the requests of this app */
static const char *const s_headers[] = {
    "HTTP-Referer", "https://github.com/vletroye/ESP32-S3-Voice-Assistant",
    "X-Title", "ESP32-S3-BOX-3 Voice Assistant",
    NULL,
};

static const cc_config_t s_config = {
    .tag = "openrouter",
    .url = "https://openrouter.ai/api/v1/chat/completions",
    // A tiny free answer (the key's limits): opens the TLS connection in advance
    .prewarm_url = "https://openrouter.ai/api/v1/key",
    .headers = s_headers,
    // Reasoning ignored by the models that do not reason
    .reasoning = CC_REASONING_OBJECT,
    .usage = CC_USAGE_INCLUDE,
    // Function calling: the device's tools (weather, radio, volume, Homey); a model refusing them answers without
    .tools = true,
};

static cc_client_t s_client = { .config = &s_config };

const llm_backend_t llm_backend_openrouter = CC_BACKEND(s_client);
