/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Phrases of the voice commands, read from one file per language: /spiffs/phrases_<code>.txt, shown on the
 * configuration drive as VOICE_<CODE>.TXT. Each list of the code is a section of the file named after its
 * variable ("[s_change_lang]"), one pattern per line, '#' for comments. A pattern stands for several phrases:
 * - "[...]": optional, e.g. "change[r] la langue" is "change la langue" and "changer la langue";
 * - "(a|b|c)": one of the choices, e.g. "change (la|de|ta) langue"; a choice may be empty: "(le |)modèle";
 * - both can be nested and combined: "je veux (éditer|modifier) (la|le) config[uration]".
 * Spaces left double or at the ends by an empty part are removed. Patterns are written as normalized by
 * app_text_normalize() (lowercase, accents kept, apostrophes and hyphens as spaces).
 *
 * Only the files of the languages of file_lang.c are read. A list takes the phrases of the current language
 * (a section missing there is taken from the English file); a list declared for all languages takes the
 * phrases of every language (e.g. "change la langue" works whatever the language). The files given with the
 * firmware (main/phrases/) are installed when missing or emptied. The phrases are given longest first: a
 * phrase that begins another one ("parle" in "parle en") never hides it.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Languages read from file_lang.c, at most */
#define PHRASES_LANG_MAX    12      /* LANG_MAX of file_lang.h */

typedef struct {
    const char *name;           /* its section in the phrase files: the name of the variable */
    bool all_languages;         /* the phrases of every language, not only of the current one */
    const char **phrases[PHRASES_LANG_MAX + 1];     /* made at the first use, by language (last: all) */
} phrase_list_t;

/* static PHRASE_LIST(s_change_model);  the section [s_change_model] of the current language's file */
#define PHRASE_LIST(var)                    phrase_list_t var = { .name = #var }
/* static PHRASE_LIST_ALL_LANGUAGES(s_change_lang);  the section [s_change_lang] of every language's file */
#define PHRASE_LIST_ALL_LANGUAGES(var)      phrase_list_t var = { .name = #var, .all_languages = true }

/**
 * @brief Install the phrase files given with the firmware where missing or emptied, and read the files of
 *        every language of file_lang.c. Once, at start-up, after SPIFFS is mounted, from a task whose stack is
 *        in internal RAM (flash access).
 */
void phrases_init(void);

/**
 * @brief The file of a language added to Languages of CONFIG.INI, made from the English one of the firmware (to be
 *        translated) when it does not exist yet
 * @return true when it was made
 */
bool phrases_create(const char *code, const char *name);

/**
 * @brief The phrases of a list in the current language (or in all of them), made at the first call.
 *
 * @return NULL-terminated array, longest phrase first (an empty list when nothing is found)
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
