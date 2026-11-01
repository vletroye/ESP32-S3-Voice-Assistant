/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * GPT client: OpenAI's Responses API (ai_provider_responses.c), with OpenAI's web search located where the user
 * lives and the device's tools.
 */

#include "llm_common.h"
#include "ai_provider_openai.h"
#include "ai_provider_responses.h"

static const rc_config_t s_config = {
    .tag = "openai_chat",
    .base_url = "https://api.openai.com/v1/",
    .search = RC_SEARCH_OPENAI,
    .web_search_cost = "openai:web_search",
    .cache_key = true,
};

static rc_client_t s_client = { .config = &s_config };

void openai_chat_set_base_url(const char *base_url)
{
    llm_str_set(&s_client.base_url, base_url);
}

const llm_backend_t llm_backend_openai = RC_BACKEND(s_client);
