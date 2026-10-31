/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Claude client (Anthropic Messages API, streaming), with the device's tools and web search. Thinking is turned
 * off when the model allows it (Haiku: disabled, Sonnet: between tool calls only); Opus always thinks, its
 * thinking blocks are kept in the history as the API requires.
 */

#pragma once

#include <stdbool.h>
#include "cJSON.h"
#include "llm_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The client for llm.c (key: Claude_key of CONFIG.INI) */
extern const llm_backend_t llm_backend_claude;

/**
 * @brief thinking.type of AGENTS.INI for the next init: "disabled", "between_tools", "default" to leave it out,
 *        NULL or "" to choose it from the model's name
 */
void claude_client_set_thinking(const char *thinking);

/**
 * @brief Runs a tool called by Claude.
 *
 * @param name     Tool name
 * @param input    Arguments (JSON object)
 * @param is_error Set to true when the result describes an error
 * @return Result given back to Claude (allocated, freed by the caller), NULL if the tool is unknown
 */
typedef char *(*claude_tool_runner_t)(const char *name, const cJSON *input, bool *is_error);

/** Offer tools: definitions (JSON array, kept by the caller) and the function running them */
void claude_client_set_tools(const cJSON *definitions, claude_tool_runner_t runner);

/** Web searches started during the current (or last) answer */
int claude_client_web_searches(void);

/** Localize the web search: 2-letter country code and town (copied), "" or NULL if unknown */
void claude_client_set_location(const char *country, const char *city);

#ifdef __cplusplus
}
#endif
