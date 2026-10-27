/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Costs of the assistant: tokens read and written by each model (transcription, answer, speech) and calls of
 * tools billed per use, priced with AGENTS.INI (currency of CONFIG.INI), by model, for today, this month and since
 * the start, plus a total per month for the last 12 months. Kept in /spiffs/costs.json, shown in COSTS.TXT.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    COST_STT = 0,           /* transcription */
    COST_LLM,               /* answer */
    COST_TTS,               /* speech */
    COST_TOOL,              /* tools billed per call (web search) */
    COST_KINDS,
} cost_kind_t;

typedef enum {
    COST_PERIOD_TODAY = 0,
    COST_PERIOD_MONTH,
    COST_PERIOD_TOTAL,
} cost_period_t;

/** Read the saved costs; after llm_load_models() (prices) and with SPIFFS mounted. currency: "EUR", "USD"... */
void costs_init(const char *currency);

/**
 * @brief Tokens of a request (any task). The prices of the model (AGENTS.INI) give its cost.
 * @param in          tokens read, without those read from or written to the cache
 * @param cache_read  tokens read from the cache (cheaper)
 * @param cache_write tokens written to the cache (Anthropic: dearer)
 * @param out         tokens written (reasoning included)
 */
void costs_add_tokens(cost_kind_t kind, const char *model, int in, int cache_read, int cache_write, int out);

/** Calls of a tool billed per use: "claude:web_search" ([per_call] of AGENTS.INI) */
void costs_add_calls(const char *tool, int calls);

/** The assistant wakes up: the cost of the conversation starts again at 0 (it stays shown while asleep) */
void costs_conversation_start(void);

/** Write costs.json and COSTS.TXT if they changed (main task, never during an exchange) */
void costs_save(void);

/**
 * @brief Cost since the assistant last woke up (the last conversation while it sleeps), by kind
 * @return its total
 */
double costs_conversation(double by_kind[COST_KINDS]);

/**
 * @brief Cost of a period, by kind, for one model (NULL: all of them)
 * @return its total
 */
double costs_period(cost_period_t period, const char *model, double by_kind[COST_KINDS]);

/** An amount with the currency: "0,0123 €", "$0.0123" (4 decimals under 1, 2 above) */
void costs_format(char *buf, size_t size, double amount, bool with_currency);

/** The same with one decimal less: "0,012 €" (3 decimals under 1, 1 above) */
void costs_format_short(char *buf, size_t size, double amount, bool with_currency);

#ifdef __cplusplus
}
#endif
