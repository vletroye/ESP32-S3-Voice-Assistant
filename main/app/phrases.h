/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Lists of voice phrases written as patterns, each standing for several phrases:
 * - "[...]": optional, e.g. "change[r] la langue" is "change la langue" and "changer la langue";
 * - "(a|b|c)": one of the choices, e.g. "change (la|de|ta) langue"; a choice may be empty: "(le |)modèle";
 * - both can be nested and combined: "je veux (éditer|modifier) (la|le) config[uration]".
 * Spaces left double or at the ends by an empty part are removed. Patterns are written as normalized by
 * app_text_normalize() (lowercase, accents kept, apostrophes and hyphens as spaces).
 * The phrases are made once, at the first use, and given longest first: a phrase that begins another one
 * ("parle" in "parle en") never hides it.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *const *patterns;    /* NULL-terminated */
    const char **phrases;           /* the phrases they stand for, NULL-terminated; made at the first use */
} phrase_list_t;

/* A static list: static phrase_list_t s_list = PHRASE_LIST("change[r] (la|de) langue", "switch language"); */
#define PHRASE_LIST(...)    { .patterns = (const char *const[]) { __VA_ARGS__, NULL }, .phrases = NULL }

/**
 * @brief The phrases of a list, made from its patterns at the first call (then kept).
 *
 * @return NULL-terminated array, longest phrase first (an empty list when out of memory)
 */
const char *const *phrases_of(phrase_list_t *list);

/**
 * @brief The phrases one pattern stands for (to check a pattern).
 *
 * @return Number of phrases written to out (at most max), each allocated (to be freed)
 */
int phrases_expand(const char *pattern, char **out, int max);

#ifdef __cplusplus
}
#endif
