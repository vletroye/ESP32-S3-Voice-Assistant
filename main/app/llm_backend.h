/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * A chat provider's client (ai_provider_<name>.c) for llm.c: what llm.c asks of every provider, one table per
 * provider (llm_backend_claude, llm_backend_openai...), in the order of llm_provider_t.
 */

#pragma once

#include "esp_err.h"
#include "cJSON.h"
#include "llm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct llm_backend {
    const char *key_name;       /* "Claude_key": named in the log when it is missing */
    void *state;                /* given back to each function (the clients sharing a code: their own state) */
    /* Configured for a model (the strings are copied); effort: of AGENTS.INI, NULL or "-" for none */
    esp_err_t (*init)(void *state, const char *api_key, const char *model, const char *instructions,
                      const char *effort);
    /* The instructions changed (language, notes to remember...): sent from the next question */
    void (*set_instructions)(void *state, const char *instructions);
    /* A new conversation */
    void (*reset)(void *state);
    /* The conversation, in the provider's format (saved by llm.c), and the one restored (taken over) */
    const cJSON *(*history)(void *state);
    void (*set_history)(void *state, cJSON *messages);
    /* While the user speaks: the TLS connection opened in advance */
    void (*prewarm)(void *state);
    /* The question answered, streamed to on_text (see llm_stream) */
    llm_error_t (*stream)(void *state, const char *text, llm_text_cb_t on_text, void *ctx, char **full_text);
} llm_backend_t;

#ifdef __cplusplus
}
#endif
