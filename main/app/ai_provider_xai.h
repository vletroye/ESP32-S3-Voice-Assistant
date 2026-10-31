/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * xAI client for the Grok models (Chat Completions API, streaming).
 */

#pragma once

#include "llm_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The client for llm.c (key: XAI_key of CONFIG.INI) */
extern const llm_backend_t llm_backend_xai;

#ifdef __cplusplus
}
#endif
