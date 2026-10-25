/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Called with each complete sentence (NUL-terminated, only valid during the call).
 */
typedef void (*sentence_cb_t)(const char *sentence, void *ctx);

typedef struct {
    char *buf;          /* text received and not emitted yet */
    size_t len;
    size_t cap;
    bool first_sent;    /* the first sentence is emitted as soon as possible */
    sentence_cb_t cb;
    void *ctx;
} sentence_splitter_t;

void sentence_splitter_init(sentence_splitter_t *s, sentence_cb_t cb, void *ctx);

/**
 * @brief Add streamed text; complete sentences are passed to the callback.
 */
void sentence_splitter_feed(sentence_splitter_t *s, const char *text);

/**
 * @brief End of text: emit what remains, then release the buffer.
 */
void sentence_splitter_finish(sentence_splitter_t *s);

/**
 * @brief Drop what remains without emitting it, then release the buffer.
 */
void sentence_splitter_discard(sentence_splitter_t *s);

#ifdef __cplusplus
}
#endif
