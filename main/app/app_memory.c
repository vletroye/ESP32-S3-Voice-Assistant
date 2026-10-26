/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "app_memory.h"
#include "phrases.h"
#include "app_lang.h"
#include "settings.h"

/* NVS strings are limited to 4000 bytes, NUL included */
#define MEMORY_MAX_LEN          3999
#define MEMORY_NORM_SIZE        512
/* A note is forgotten when this share of the words of "oublie que ..." is found in it */
#define FORGET_MATCH_PERCENT    60

static const char *TAG = "memory";

/*
 * Phrases as normalized by app_text_normalize(), written as patterns (phrases.h: "[optional]",
 * "(choice|choice)"); French and English are both accepted
 */
static phrase_list_t s_remember = PHRASE_LIST(
    "souviens toi[ bien] que", "rappelle toi que", "retiens[ bien] que", "n oublie pas que", "note que",
    "m(é|e)morise que", "[please ]remember that", "don t forget that", "note that", "memorize that"
);
static phrase_list_t s_forget = PHRASE_LIST(
    "oublie[ le fait] que", "forget that"
);
static phrase_list_t s_forget_all = PHRASE_LIST(
    "oublie tout", "efface[ toute] ta mémoire", "forget everything"
);
static phrase_list_t s_list = PHRASE_LIST(
    "dis moi[ tout] ce que tu as (mémorisé|retenu)", "qu est ce que tu as (mémorisé|retenu)", "qu as tu mémorisé",
    "tell me everything you remember", "what do you remember", "what have you memorized"
);

/* MEMORY.TXT as saved (comment lines included), and the notes alone, as given to the assistant */
static char *s_file = NULL;
static char *s_notes = NULL;

void app_text_normalize(const char *text, char *out, size_t size)
{
    size_t n = 0;
    for (const char *p = text; *p && n < size - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || (c & 0x80)) {
            // UTF-8 Latin-1 capitals (C3 80..C3 9E, e.g. "À") to lowercase, like ASCII
            bool latin1_upper = n > 0 && (unsigned char)out[n - 1] == 0xC3 && c >= 0x80 && c <= 0x9E && c != 0x97;
            out[n++] = latin1_upper ? c + 0x20 : tolower(c);
        } else if (n > 0 && out[n - 1] != ' ') {
            out[n++] = ' ';
        }
    }
    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }
    out[n] = '\0';
}

static void set_file(const char *file)
{
    free(s_file);
    s_file = strdup(file ? file : "");
    free(s_notes);
    s_notes = settings_text_without_comments(s_file);
}

void app_memory_init(const char *notes)
{
    // The phrases of the memory commands, made from their patterns now rather than at the first command
    phrases_of(&s_remember);
    phrases_of(&s_forget);
    phrases_of(&s_forget_all);
    phrases_of(&s_list);
    set_file(notes);
    ESP_LOGI(TAG, "%u bytes of notes (%u with the comments)", (unsigned)strlen(app_memory_notes()),
             (unsigned)(s_file ? strlen(s_file) : 0));
}

static const char *memory_file(void)
{
    return s_file ? s_file : "";
}

/* A comment line of MEMORY.TXT (kept in the file, never a note) */
static bool is_comment(const char *line)
{
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    return *line == '#';
}

const char *app_memory_notes(void)
{
    return s_notes ? s_notes : "";
}

static bool is_word_char(unsigned char c)
{
    return isalnum(c) || (c & 0x80);
}

/* Number of words of a normalized phrase */
static int word_count(const char *norm)
{
    int words = norm[0] ? 1 : 0;
    for (const char *p = norm; *p; p++) {
        words += (*p == ' ');
    }
    return words;
}

/* Phrase of the list that the normalized text is ("exact") or starts with (followed by more words) */
static const char *match(const char *norm, const char *const *phrases, bool exact)
{
    for (; *phrases; phrases++) {
        size_t len = strlen(*phrases);
        if (strncmp(norm, *phrases, len) == 0 && (exact ? norm[len] == '\0' : norm[len] == ' ')) {
            return *phrases;
        }
    }
    return NULL;
}

/* The original text after its first "words" words, without the final punctuation */
static char *text_after_words(const char *text, int words)
{
    const char *p = text;
    for (int w = 0; w < words; w++) {
        while (*p && !is_word_char((unsigned char)*p)) {
            p++;
        }
        while (*p && is_word_char((unsigned char)*p)) {
            p++;
        }
    }
    while (*p && !is_word_char((unsigned char)*p)) {
        p++;
    }
    char *rest = strdup(p);
    if (rest) {
        size_t n = strlen(rest);
        while (n > 0 && (isspace((unsigned char)rest[n - 1]) || strchr(".!?", rest[n - 1]))) {
            rest[--n] = '\0';
        }
    }
    return rest;
}

/* The whole file (comment lines included) */
static bool save_notes(const char *file)
{
    if (settings_save_str("Memory", file) != ESP_OK) {
        return false;
    }
    set_file(file);
    return true;
}

static memory_cmd_t remember(const char *fact, char **reply)
{
    const app_lang_t *lang = app_lang_get();
    char date[16] = "";
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year + 1900 >= 2024) {
        strftime(date, sizeof(date), " (%d/%m/%y)", &t);
    }
    char *notes = NULL;
    asprintf(&notes, "%s%s- %s%s", memory_file(), memory_file()[0] ? "\n" : "", fact, date);
    if (!notes || strlen(notes) > MEMORY_MAX_LEN) {
        free(notes);
        *reply = strdup(lang->memory_full);
        return MEMORY_CMD_ANSWERED;
    }
    bool saved = save_notes(notes);
    free(notes);
    ESP_LOGI(TAG, "remembered: \"%s\"", fact);
    *reply = strdup(saved ? lang->memory_saved : lang->memory_full);
    return saved ? MEMORY_CMD_CHANGED : MEMORY_CMD_ANSWERED;
}

/* Remove the note sharing the most words (3 letters or more) with what is to be forgotten */
static memory_cmd_t forget(const char *what, char **reply)
{
    const app_lang_t *lang = app_lang_get();
    char *wanted = malloc(MEMORY_NORM_SIZE);
    char *line_norm = malloc(MEMORY_NORM_SIZE);
    char *notes = strdup(memory_file());
    memory_cmd_t result = MEMORY_CMD_ANSWERED;
    *reply = NULL;
    if (!wanted || !line_norm || !notes) {
        goto end;
    }
    app_text_normalize(what, wanted, MEMORY_NORM_SIZE);

    char *best = NULL;
    size_t best_len = 0;
    int best_percent = 0;
    for (char *line = notes; *line;) {
        char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (is_comment(line)) {
            line += len + (end ? 1 : 0);    // comments are never forgotten
            continue;
        }
        char saved = line[len];
        line[len] = '\0';
        app_text_normalize(line, line_norm, MEMORY_NORM_SIZE);
        line[len] = saved;

        int words = 0;
        int found = 0;
        for (char *w = wanted; *w;) {
            char *space = strchr(w, ' ');
            size_t wlen = space ? (size_t)(space - w) : strlen(w);
            if (wlen >= 3) {
                words++;
                char word[64];
                snprintf(word, sizeof(word), "%.*s", (int)(wlen < sizeof(word) - 1 ? wlen : sizeof(word) - 1), w);
                found += (strstr(line_norm, word) != NULL);
            }
            w += wlen + (space ? 1 : 0);
        }
        int percent = words ? found * 100 / words : 0;
        if (percent > best_percent) {
            best_percent = percent;
            best = line;
            best_len = len;
        }
        line += len + (end ? 1 : 0);
    }

    if (best && best_percent >= FORGET_MATCH_PERCENT) {
        // Remove the line and its line break
        size_t remove = best_len + (best[best_len] == '\n' ? 1 : 0);
        if (best[best_len] != '\n' && best > notes) {
            best--;     // last line: remove the line break before it
            remove++;
        }
        memmove(best, best + remove, strlen(best + remove) + 1);
        if (save_notes(notes)) {
            ESP_LOGI(TAG, "forgotten (%d%% of the words): \"%s\"", best_percent, what);
            result = MEMORY_CMD_CHANGED;
            *reply = strdup(lang->memory_forgotten);
        }
    } else {
        *reply = strdup(lang->memory_not_found);
    }

end:
    if (!*reply) {
        *reply = strdup(lang->memory_not_found);
    }
    free(wanted);
    free(line_norm);
    free(notes);
    return result;
}

/* Spoken list: the notes without the "- " and the dates */
static memory_cmd_t list(char **reply)
{
    const app_lang_t *lang = app_lang_get();
    const char *notes = app_memory_notes();
    if (notes[0] == '\0') {
        *reply = strdup(lang->memory_empty);
        return MEMORY_CMD_ANSWERED;
    }
    size_t size = strlen(lang->memory_list_intro) + strlen(notes) * 2 + 8;
    char *out = malloc(size);
    if (!out) {
        *reply = strdup(lang->memory_empty);
        return MEMORY_CMD_ANSWERED;
    }
    strlcpy(out, lang->memory_list_intro, size);
    for (const char *line = notes; *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        const char *start = line;
        if (len >= 2 && start[0] == '-' && start[1] == ' ') {
            start += 2;
            len -= 2;
        }
        const char *date = memchr(start, '(', len);
        size_t text_len = date ? (size_t)(date - start) : len;
        while (text_len > 0 && isspace((unsigned char)start[text_len - 1])) {
            text_len--;
        }
        if (text_len > 0) {
            size_t n = strlen(out);
            snprintf(out + n, size - n, "%.*s. ", (int)text_len, start);
        }
        line += (end ? (size_t)(end - line) + 1 : strlen(line));
    }
    *reply = out;
    return MEMORY_CMD_ANSWERED;
}

memory_cmd_t app_memory_handle(const char *text, char **reply)
{
    *reply = NULL;
    char *norm = malloc(MEMORY_NORM_SIZE);
    if (!norm) {
        return MEMORY_CMD_NONE;
    }
    app_text_normalize(text, norm, MEMORY_NORM_SIZE);

    memory_cmd_t result = MEMORY_CMD_NONE;
    const char *phrase;
    if (match(norm, phrases_of(&s_list), true)) {
        result = list(reply);
    } else if (match(norm, phrases_of(&s_forget_all), true)) {
        // The notes go, the comment lines stay
        char *comments = calloc(1, strlen(memory_file()) + 1);
        for (const char *line = memory_file(); comments && *line;) {
            const char *end = strchr(line, '\n');
            size_t len = end ? (size_t)(end - line + 1) : strlen(line);
            if (is_comment(line)) {
                strncat(comments, line, len);
            }
            line += len;
        }
        size_t n = comments ? strlen(comments) : 0;
        while (n > 0 && (unsigned char)comments[n - 1] <= ' ') {
            comments[--n] = '\0';
        }
        if (comments && save_notes(comments)) {
            ESP_LOGI(TAG, "all notes forgotten");
            *reply = strdup(app_lang_get()->memory_cleared);
            result = MEMORY_CMD_CHANGED;
        }
        free(comments);
    } else if ((phrase = match(norm, phrases_of(&s_remember), false)) != NULL) {
        char *fact = text_after_words(text, word_count(phrase));
        if (fact && fact[0]) {
            result = remember(fact, reply);
        }
        free(fact);
    } else if ((phrase = match(norm, phrases_of(&s_forget), false)) != NULL) {
        char *what = text_after_words(text, word_count(phrase));
        if (what && what[0]) {
            result = forget(what, reply);
        }
        free(what);
    }
    free(norm);
    return result;
}
