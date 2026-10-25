/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Splits the streamed answer into sentences for speech synthesis. The first sentence is emitted as
 * soon as it is complete (shortest time to the first sound); the next ones are grouped up to a
 * minimum length so that each synthesis request carries enough text for a natural prosody.
 */

#include <ctype.h>
#include <string.h>
#include <strings.h>
#include "esp_heap_caps.h"
#include "sentence_splitter.h"

#define FIRST_SENTENCE_MIN_LEN  12
#define SENTENCE_MIN_LEN        60
/* Without punctuation, cut at a comma or a space beyond this length */
#define SENTENCE_MAX_LEN        240

/* A period after these words does not end a sentence */
static const char *const s_abbreviations[] = {
    "M", "MM", "Mme", "Mmes", "Mlle", "Mlles", "Dr", "Pr", "St", "Ste", "av", "bd", "cf", "env",
    "ex", "fig", "min", "max", "no", "p", "vol", "Mr", "Mrs", "Ms", "vs", "approx",
};

void sentence_splitter_init(sentence_splitter_t *s, sentence_cb_t cb, void *ctx)
{
    memset(s, 0, sizeof(*s));
    s->cb = cb;
    s->ctx = ctx;
}

static void append(sentence_splitter_t *s, const char *text, size_t n)
{
    if (s->len + n + 1 > s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 256;
        while (cap < s->len + n + 1) {
            cap *= 2;
        }
        char *p = heap_caps_realloc(s->buf, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) {
            return;
        }
        s->buf = p;
        s->cap = cap;
    }
    memcpy(s->buf + s->len, text, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

/* True if the period at buf[dot] follows an abbreviation or an initial ("J. Dupont") */
static bool is_abbreviation(const char *buf, size_t dot)
{
    size_t start = dot;
    while (start > 0 && isalpha((unsigned char)buf[start - 1])) {
        start--;
    }
    size_t word_len = dot - start;
    if (word_len == 0) {
        return false;
    }
    if (word_len == 1 && isupper((unsigned char)buf[start])) {
        return true;
    }
    for (size_t i = 0; i < sizeof(s_abbreviations) / sizeof(s_abbreviations[0]); i++) {
        if (strlen(s_abbreviations[i]) == word_len && strncmp(buf + start, s_abbreviations[i], word_len) == 0) {
            return true;
        }
    }
    return false;
}

/* Emit buf[0..end) (trimmed) and keep the rest */
static void emit(sentence_splitter_t *s, size_t end)
{
    size_t from = 0;
    while (from < end && isspace((unsigned char)s->buf[from])) {
        from++;
    }
    size_t to = end;
    while (to > from && isspace((unsigned char)s->buf[to - 1])) {
        to--;
    }
    if (to > from) {
        char saved = s->buf[to];
        s->buf[to] = '\0';
        s->cb(s->buf + from, s->ctx);
        s->buf[to] = saved;
        s->first_sent = true;
    }
    memmove(s->buf, s->buf + end, s->len - end + 1);
    s->len -= end;
}

/*
 * Position just after the last sentence end that is followed by a space, or 0. A sentence end is
 * '.', '!', '?', an ellipsis or a line break, possibly followed by closing quotes or brackets.
 */
static size_t last_sentence_end(const sentence_splitter_t *s, size_t min_len)
{
    size_t best = 0;
    for (size_t i = 0; i + 1 < s->len; i++) {
        char c = s->buf[i];
        bool end = false;
        if (c == '\n') {
            end = true;
        } else if (c == '!' || c == '?') {
            end = true;
        } else if (c == '.') {
            end = !is_abbreviation(s->buf, i);
        } else if ((unsigned char)c == 0xE2 && i + 2 < s->len &&
                   (unsigned char)s->buf[i + 1] == 0x80 && (unsigned char)s->buf[i + 2] == 0xA6) {
            i += 2;     /* "…" */
            end = true;
        }
        if (!end) {
            continue;
        }
        size_t j = i + 1;
        /* Closing quotes, brackets or a French closing guillemet (U+00BB, then a no-break space) */
        while (j < s->len && (s->buf[j] == '"' || s->buf[j] == ')' || s->buf[j] == '\'' ||
                              ((unsigned char)s->buf[j] == 0xC2 && j + 1 < s->len &&
                               ((unsigned char)s->buf[j + 1] == 0xBB || (unsigned char)s->buf[j + 1] == 0xA0)))) {
            j += ((unsigned char)s->buf[j] == 0xC2) ? 2 : 1;
        }
        /* The end is only certain once the following space has arrived ("3.5", "...") */
        if (j < s->len && isspace((unsigned char)s->buf[j]) && j >= min_len) {
            best = j;
        }
    }
    return best;
}

void sentence_splitter_feed(sentence_splitter_t *s, const char *text)
{
    append(s, text, strlen(text));
    for (;;) {
        size_t min_len = s->first_sent ? SENTENCE_MIN_LEN : FIRST_SENTENCE_MIN_LEN;
        size_t end = last_sentence_end(s, min_len);
        if (end == 0 && s->len > SENTENCE_MAX_LEN) {
            /* Long text without a sentence end: cut after the last comma, else the last space */
            for (size_t i = s->len - 1; i > SENTENCE_MIN_LEN && end == 0; i--) {
                if ((s->buf[i - 1] == ',' || s->buf[i - 1] == ';' || s->buf[i - 1] == ':') && isspace((unsigned char)s->buf[i])) {
                    end = i;
                }
            }
            for (size_t i = s->len - 1; i > SENTENCE_MIN_LEN && end == 0; i--) {
                if (isspace((unsigned char)s->buf[i])) {
                    end = i;
                }
            }
        }
        if (end == 0) {
            return;
        }
        emit(s, end);
    }
}

void sentence_splitter_discard(sentence_splitter_t *s)
{
    free(s->buf);
    s->buf = NULL;
    s->len = s->cap = 0;
}

void sentence_splitter_finish(sentence_splitter_t *s)
{
    if (s->len) {
        emit(s, s->len);
    }
    sentence_splitter_discard(s);
}
