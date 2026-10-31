/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * GPT client (OpenAI Responses API, streaming).
 */

#pragma once

#include "llm_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The client for llm.c (key: OpenAI_key of CONFIG.INI) */
extern const llm_backend_t llm_backend_openai;

/** OpenAI base URL ("https://api.openai.com/v1/", OpenAI_url of CONFIG.INI), copied: set before its init */
void openai_chat_set_base_url(const char *base_url);

#ifdef __cplusplus
}
#endif
