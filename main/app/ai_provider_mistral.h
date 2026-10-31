/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Mistral AI client (Chat Completions API, streaming).
 */

#pragma once

#include "llm_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The client for llm.c (key: Mistral_key of CONFIG.INI) */
extern const llm_backend_t llm_backend_mistral;

#ifdef __cplusplus
}
#endif
