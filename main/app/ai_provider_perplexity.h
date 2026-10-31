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

#ifdef __cplusplus
}
#endif
