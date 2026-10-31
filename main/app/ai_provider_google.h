/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Gemini client (Gemini API, streamGenerateContent).
 */

#pragma once

#include "llm_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The client for llm.c (key: Gemini_key of CONFIG.INI) */
extern const llm_backend_t llm_backend_gemini;

#ifdef __cplusplus
}
#endif
