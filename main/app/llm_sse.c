/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdlib.h>
#include <string.h>
#include "llm_sse.h"

bool llm_buf_append(llm_buf_t *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) {
            cap *= 2;
        }
        char *p = realloc(b->p, cap);
        if (!p) {
            return false;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return true;
}

void llm_buf_clear(llm_buf_t *b)
{
    b->len = 0;
    if (b->p) {
        b->p[0] = '\0';
    }
}

void llm_buf_free(llm_buf_t *b)
{
    free(b->p);
    memset(b, 0, sizeof(*b));
}

void llm_sse_feed(llm_sse_t *sse, const char *bytes, int len)
{
    for (int i = 0; i < len; i++) {
        char c = bytes[i];
        if (c != '\n') {
            if (c != '\r') {
                llm_buf_append(&sse->line, &c, 1);
            }
            continue;
        }
        if (sse->line.len == 0) {
            if (sse->data.len) {
                sse->on_event(sse->data.p, sse->ctx);
            }
            llm_buf_clear(&sse->data);
        } else if (strncmp(sse->line.p, "data:", 5) == 0) {
            const char *payload = sse->line.p + 5;
            if (*payload == ' ') {
                payload++;
            }
            if (sse->data.len) {
                llm_buf_append(&sse->data, "\n", 1);
            }
            llm_buf_append(&sse->data, payload, strlen(payload));
        }
        /* "event:" lines are redundant with the JSON "type" field; ":" comments are ignored */
        llm_buf_clear(&sse->line);
    }
}

void llm_sse_free(llm_sse_t *sse)
{
    llm_buf_free(&sse->line);
    llm_buf_free(&sse->data);
}
