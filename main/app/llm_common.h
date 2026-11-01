/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Helpers shared by the chat clients (ai_provider_<name>.c): settings strings, the conversation history kept on
 * the device, the answer being streamed, and the HTTPS connection kept alive between questions.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "llm.h"
#include "llm_sse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Added to the instructions when the model can search the web (spoken answers: no addresses read out) */
#define LLM_WEB_SEARCH_HINT     "You can search the web for current information (news, results, schedules, " \
                                "prices, opening hours...). Before searching, say a very short sentence in the " \
                                "user's language, such as \"Je regarde.\". Never read out web addresses, " \
                                "citations or source names unless asked."

/** "instructions\n\nLLM_WEB_SEARCH_HINT" (allocated), NULL without instructions or memory */
char *llm_with_web_hint(const char *instructions);

/** A web search started by the provider for this answer: counted (the web icon of the answer, llm_web_searches) */
void llm_web_search_started(void);

/* ---------- strings ---------- */

/** *dst replaced by a copy of src; NULL for NULL or "" */
void llm_str_set(char **dst, const char *src);

/** "Bearer <key>" into *dst (allocated, the previous one freed); false without memory */
bool llm_bearer_set(char **dst, const char *api_key);

/* ---------- conversation history: a cJSON array of messages ---------- */

/*
 * History kept for follow-up questions: about the last 10 exchanges. Past the maximum it is cut down to
 * LLM_HISTORY_TRIM_TO in one go: its start then stays the same for several questions, and so does the providers'
 * prompt cache (dropping the oldest exchange at every question would change the prefix of every request)
 */
#define LLM_HISTORY_MAX_MSGS    24
#define LLM_HISTORY_TRIM_TO     12

/** An empty history in place of *messages */
void llm_history_reset(cJSON **messages);

/** *messages replaced by messages (taken over), trimmed */
void llm_history_set(cJSON **messages, cJSON *taken);

/**
 * @brief Older messages dropped in pairs (question, answer) past LLM_HISTORY_MAX_MSGS.
 *
 * @param cannot_start NULL, or true for a message the history cannot start with (dropped too, in pairs)
 */
void llm_history_trim(cJSON *messages, bool (*cannot_start)(const cJSON *message));

/** {"role": role, "content": text} appended */
void llm_history_add(cJSON *messages, const char *role, const char *text);

/** The messages after the first count removed (an exchange that failed) */
void llm_history_drop_from(cJSON *messages, int count);

/* ---------- the answer being streamed ---------- */

typedef struct {
    llm_text_cb_t on_text;      /* shows and speaks each piece, NULL for none (model test) */
    void *ctx;
    llm_buf_t text;             /* whole answer text */
    int64_t start_us;           /* the request sent */
    int64_t first_text_us;      /* its first words, 0 before */
} llm_answer_t;

/** Start of an answer: its start time set */
void llm_answer_begin(llm_answer_t *a, llm_text_cb_t on_text, void *ctx);

/** A piece of the answer (len bytes, not necessarily NUL-terminated): kept, timed and given to on_text */
void llm_answer_add(llm_answer_t *a, const char *text, size_t len);

/** Until the first words in ms, -1 if none */
int llm_answer_first_ms(const llm_answer_t *a);

/** Since the request in ms */
int llm_answer_total_ms(const llm_answer_t *a);

/** llm_last_stats from the answer's times and these tokens (reasoning: -1 when not given apart) */
void llm_answer_stats(const llm_answer_t *a, int answer_tokens, int reasoning_tokens);

/* ---------- HTTPS connection kept alive between questions ---------- */

typedef struct {
    const char *tag;                /* of the logs */
    const char *url;                /* the POST endpoint (may be a buffer of the client) */
    const char *key_header;         /* "Authorization", "x-api-key", "x-goog-api-key" */
    const char *key;                /* its value (owned by the client), NULL before the configuration */
    const char *const *headers;     /* more headers: name, value, ..., NULL; NULL for none */
    esp_http_client_handle_t client;
} llm_http_t;

/** The connection (made on first use, kept alive), its key header set; NULL without memory */
esp_http_client_handle_t llm_http_client(llm_http_t *h);

/** The connection closed (a new one is made by the next request) */
void llm_http_drop(llm_http_t *h);

/** POST of body, retried once on a fresh connection if a kept-alive one went stale; *status: the HTTP status */
esp_err_t llm_http_send(llm_http_t *h, const char *body, int *status);

/**
 * @brief A free GET with a small answer, made while the user speaks: it opens the TLS connection, kept for the
 *        question. Its body is read to its end (into body when not NULL, up to max bytes).
 *
 * @return The HTTP status, -1 without answer
 */
int llm_http_prewarm(llm_http_t *h, const char *url, llm_buf_t *body, size_t max);

/** An error answer's JSON ({"error": ...}), or NULL when it was not JSON: its kind for the user */
typedef llm_error_t (*llm_error_parse_t)(const void *ctx, int status, const cJSON *json);

/**
 * @brief The body of an answer that is not 200, read whole, then the connection dropped: its error.
 *
 * @param ctx  Given to parse
 * @param body NULL, or the raw body kept for the caller (llm_buf_free it)
 */
llm_error_t llm_http_error(llm_http_t *h, int status, llm_error_parse_t parse, const void *ctx, llm_buf_t *body);

/** The event stream read into sse until *done (NULL: to its end), the end of the body or llm_cancel() */
void llm_http_stream(llm_http_t *h, llm_sse_t *sse, const bool *done);

/** After the stream: the rest of the body read (the connection reused by the next question), else dropped */
void llm_http_finish(llm_http_t *h, bool ok);

#ifdef __cplusplus
}
#endif
