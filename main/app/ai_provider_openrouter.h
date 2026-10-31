/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * OpenRouter client (Chat Completions API, streaming): one key gives access to the models of many
 * providers ("vendor/model").
 */

#pragma once

#include "llm_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The client for llm.c (key: OpenRouter_key of CONFIG.INI) */
extern const llm_backend_t llm_backend_openrouter;

#ifdef __cplusplus
}
#endif
