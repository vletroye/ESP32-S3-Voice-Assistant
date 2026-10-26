/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "phrases.h"

/* Phrases one pattern may stand for: far more than needed, a guard against a mistyped pattern */
#define PHRASES_PER_PATTERN_MAX     256
#define PHRASE_MAX_LEN              160

static const char *TAG = "phrases";

/* Phrases being built */
typedef struct {
    char **items;
    int count;
} phrase_set_t;

static void set_free(phrase_set_t *set)
{
    for (int i = 0; i < set->count; i++) {
        free(set->items[i]);
    }
    free(set->items);
    set->items = NULL;
    set->count = 0;
}

static bool set_add(phrase_set_t *set, const char *a, size_t a_len, const char *b, size_t b_len)
{
    if (set->count >= PHRASES_PER_PATTERN_MAX || a_len + b_len >= PHRASE_MAX_LEN) {
        return false;
    }
    char **items = realloc(set->items, (set->count + 1) * sizeof(char *));
    char *phrase = malloc(a_len + b_len + 1);
    if (!items || !phrase) {
        free(phrase);
        if (items) {
            set->items = items;
        }
        return false;
    }
    memcpy(phrase, a, a_len);
    memcpy(phrase + a_len, b, b_len);
    phrase[a_len + b_len] = '\0';
    set->items = items;
    set->items[set->count++] = phrase;
    return true;
}

/* End of the group opened at p[0] ('(' or '['): the matching closing bracket, NULL if none */
static const char *group_end(const char *p, const char *end)
{
    int depth = 0;
    for (const char *c = p; c < end; c++) {
        if (*c == '(' || *c == '[') {
            depth++;
        } else if ((*c == ')' || *c == ']') && --depth == 0) {
            return c;
        }
    }
    return NULL;
}

static bool expand_range(const char *p, const char *end, phrase_set_t *out);

/* Every phrase of `prefixes` followed by every phrase of `suffixes`, into *prefixes */
static bool combine(phrase_set_t *prefixes, phrase_set_t *suffixes)
{
    phrase_set_t combined = {0};
    bool ok = true;
    for (int i = 0; i < prefixes->count && ok; i++) {
        for (int j = 0; j < suffixes->count && ok; j++) {
            ok = set_add(&combined, prefixes->items[i], strlen(prefixes->items[i]), suffixes->items[j],
                         strlen(suffixes->items[j]));
        }
    }
    set_free(prefixes);
    *prefixes = combined;
    return ok;
}

/* The choices of a group's content (top-level '|'), each expanded, into out; "[...]" adds the empty choice */
static bool expand_group(const char *p, const char *end, bool optional, phrase_set_t *out)
{
    bool ok = true;
    const char *start = p;
    int depth = 0;
    for (const char *c = p; c <= end && ok; c++) {
        if (c < end && (*c == '(' || *c == '[')) {
            depth++;
        } else if (c < end && (*c == ')' || *c == ']')) {
            depth--;
        } else if (c == end || (*c == '|' && depth == 0)) {
            ok = expand_range(start, c, out);
            start = c + 1;
        }
    }
    if (ok && optional) {
        ok = set_add(out, "", 0, "", 0);
    }
    return ok;
}

/* The phrases of p[0..end), appended to out */
static bool expand_range(const char *p, const char *end, phrase_set_t *out)
{
    phrase_set_t prefixes = {0};
    bool ok = set_add(&prefixes, "", 0, "", 0);
    while (ok && p < end) {
        if (*p == '(' || *p == '[') {
            const char *close = group_end(p, end);
            if (!close) {
                ok = false;
                break;
            }
            phrase_set_t choices = {0};
            ok = expand_group(p + 1, close, *p == '[', &choices) && combine(&prefixes, &choices);
            set_free(&choices);
            p = close + 1;
        } else {
            // Text up to the next group, added to every prefix
            const char *text_end = p;
            while (text_end < end && *text_end != '(' && *text_end != '[') {
                text_end++;
            }
            phrase_set_t text = {0};
            ok = set_add(&text, p, (size_t)(text_end - p), "", 0) && combine(&prefixes, &text);
            set_free(&text);
            p = text_end;
        }
    }
    for (int i = 0; i < prefixes.count && ok; i++) {
        ok = set_add(out, prefixes.items[i], strlen(prefixes.items[i]), "", 0);
    }
    set_free(&prefixes);
    return ok;
}

/* Single spaces only, none at the ends (an empty optional part leaves "change  langue") */
static void tidy_spaces(char *s)
{
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (*r == ' ' && (w == s || w[-1] == ' ')) {
            continue;
        }
        *w++ = *r;
    }
    while (w > s && w[-1] == ' ') {
        w--;
    }
    *w = '\0';
}

int phrases_expand(const char *pattern, char **out, int max)
{
    phrase_set_t set = {0};
    if (!expand_range(pattern, pattern + strlen(pattern), &set)) {
        ESP_LOGE(TAG, "Pattern left out (unbalanced brackets or too many phrases): \"%s\"", pattern);
        set_free(&set);
        return 0;
    }
    int n = 0;
    for (int i = 0; i < set.count; i++) {
        tidy_spaces(set.items[i]);
        if (n < max && set.items[i][0]) {
            out[n++] = set.items[i];
            set.items[i] = NULL;
        }
    }
    set_free(&set);
    return n;
}

const char *const *phrases_of(phrase_list_t *list)
{
    static const char *const s_empty[] = { NULL };
    if (list->phrases) {
        return list->phrases;
    }
    int count = 0;
    int patterns = 0;
    char **all = NULL;
    for (const char *const *pattern = list->patterns; *pattern; pattern++, patterns++) {
        static char *made[PHRASES_PER_PATTERN_MAX];     // static: kept off the caller's stack
        int n = phrases_expand(*pattern, made, PHRASES_PER_PATTERN_MAX);
        char **grown = realloc(all, (count + n + 1) * sizeof(char *));
        if (!grown) {
            for (int i = 0; i < n; i++) {
                free(made[i]);
            }
            continue;
        }
        all = grown;
        for (int i = 0; i < n; i++) {
            bool known = false;
            for (int j = 0; j < count && !known; j++) {
                known = (strcmp(all[j], made[i]) == 0);
            }
            if (known) {
                free(made[i]);
            } else {
                all[count++] = made[i];
            }
        }
    }
    if (!all) {
        return s_empty;
    }
    // Longest first, in the written order otherwise (stable insertion sort: the lists are short)
    for (int i = 1; i < count; i++) {
        char *phrase = all[i];
        size_t len = strlen(phrase);
        int j = i - 1;
        while (j >= 0 && strlen(all[j]) < len) {
            all[j + 1] = all[j];
            j--;
        }
        all[j + 1] = phrase;
    }
    all[count] = NULL;
    ESP_LOGI(TAG, "%d phrases from %d patterns (\"%s\"...)", count, patterns, list->patterns[0]);
    list->phrases = (const char **)all;
    return list->phrases;
}
