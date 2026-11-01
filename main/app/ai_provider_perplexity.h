/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Perplexity client (Sonar, Agent API, streaming, with its web search).
 */

#pragma once

#include "llm_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The client for llm.c (key: Perplexity_key of CONFIG.INI) */
extern const llm_backend_t llm_backend_perplexity;

/**
 * @brief The web_search tool of the models without a web search of their own: the question answered by Perplexity
 *        (the first model of [perplexity], Sonar), in a few sentences without sources. Allocated, NULL without
 *        Perplexity_key or on failure. Not to be called while a Perplexity model holds the conversation.
 */
char *perplexity_search(const char *query);

#ifdef __cplusplus
}
#endif
