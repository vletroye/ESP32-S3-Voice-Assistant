/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Helpers shared by the streaming chat clients: growable string buffer and server-sent events parser.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *p;
    size_t len;
    size_t cap;
} llm_buf_t;

bool llm_buf_append(llm_buf_t *b, const char *s, size_t n);
void llm_buf_clear(llm_buf_t *b);
void llm_buf_free(llm_buf_t *b);

/** Called with the data: payload of each complete event (NUL-terminated, only valid during the call) */
typedef void (*llm_sse_event_cb_t)(const char *data, void *ctx);

typedef struct {
    llm_buf_t line;             /* line being assembled */
    llm_buf_t data;             /* data: payload of the current event */
    llm_sse_event_cb_t on_event;
    void *ctx;
} llm_sse_t;

/** Feed raw bytes of the event stream: lines "event: ..." / "data: ...", events end with a blank line */
void llm_sse_feed(llm_sse_t *sse, const char *bytes, int len);

void llm_sse_free(llm_sse_t *sse);

#ifdef __cplusplus
}
#endif
