/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "file_lang.h"
#include "file_phrases.h"

/* Phrases one pattern may stand for: far more than needed, a guard against a mistyped pattern */
#define PHRASES_PER_PATTERN_MAX     256
#define PHRASE_MAX_LEN              160

static const char *TAG = "phrases";

/*
 * The phrases (hundreds of short strings, kept while the device runs) go to PSRAM: a plain malloc() would put
 * these small blocks in the internal RAM, which TLS and the tasks need. Freed with free().
 */
static void *ps_realloc(void *ptr, size_t size)
{
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
}

static char *ps_strndup(const char *s, size_t len)
{
    char *copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (copy) {
        memcpy(copy, s, len);
        copy[len] = '\0';
    }
    return copy;
}

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
    char **items = ps_realloc(set->items, (set->count + 1) * sizeof(char *));
    char *phrase = heap_caps_malloc(a_len + b_len + 1, MALLOC_CAP_SPIRAM);
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

/* ---------- phrase files ---------- */

#define PHRASES_FILE_FORMAT     "/spiffs/phrases_%s.txt"
#define PHRASES_FILE_MAX        16384   /* as on the configuration drive */
#define PHRASES_FALLBACK_LANG   "en"
/* Patterns of one list, at most */
#define PATTERNS_PER_LIST_MAX   64

/* Files given with the firmware (main/phrases/, embedded NUL-terminated), installed when missing or emptied */
extern const char s_default_fr_start[] asm("_binary_phrases_fr_txt_start");
extern const char s_default_fr_end[] asm("_binary_phrases_fr_txt_end");
extern const char s_default_en_start[] asm("_binary_phrases_en_txt_start");
extern const char s_default_en_end[] asm("_binary_phrases_en_txt_end");
extern const char s_default_nl_start[] asm("_binary_phrases_nl_txt_start");
extern const char s_default_nl_end[] asm("_binary_phrases_nl_txt_end");
extern const char s_default_de_start[] asm("_binary_phrases_de_txt_start");
extern const char s_default_de_end[] asm("_binary_phrases_de_txt_end");
extern const char s_default_es_start[] asm("_binary_phrases_es_txt_start");
extern const char s_default_es_end[] asm("_binary_phrases_es_txt_end");
extern const char s_default_it_start[] asm("_binary_phrases_it_txt_start");
extern const char s_default_it_end[] asm("_binary_phrases_it_txt_end");
static const struct {
    const char *code;
    const char *start;
    const char *end;
} s_defaults[] = {
    { "fr", s_default_fr_start, s_default_fr_end },
    { "en", s_default_en_start, s_default_en_end },
    { "nl", s_default_nl_start, s_default_nl_end },
    { "de", s_default_de_start, s_default_de_end },
    { "es", s_default_es_start, s_default_es_end },
    { "it", s_default_it_start, s_default_it_end },
};

/* Text of the file of each language of file_lang.c (same index), read at start-up */
static char *s_texts[PHRASES_LANG_MAX];
static size_t s_lang_count = 0;
/* The lists are made in the task that first uses them */
static SemaphoreHandle_t s_lock = NULL;

static bool is_blank_text(const char *text)
{
    for (; *text; text++) {
        if ((unsigned char)*text > ' ') {
            return false;
        }
    }
    return true;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }
    char *text = heap_caps_malloc(PHRASES_FILE_MAX, MALLOC_CAP_SPIRAM);
    size_t n = text ? fread(text, 1, PHRASES_FILE_MAX - 1, f) : 0;
    fclose(f);
    if (text) {
        text[n] = '\0';
    }
    return text;
}

static void install_default(const char *code, const char *path)
{
    for (size_t i = 0; i < sizeof(s_defaults) / sizeof(s_defaults[0]); i++) {
        if (strcasecmp(code, s_defaults[i].code) != 0) {
            continue;
        }
        size_t len = (size_t)(s_defaults[i].end - s_defaults[i].start) - 1;     // without the added NUL
        FILE *f = fopen(path, "w");
        bool ok = f && fwrite(s_defaults[i].start, 1, len, f) == len;
        if (f) {
            ok &= (fclose(f) == 0);
        }
        ESP_LOGI(TAG, "%s %s (%u bytes)", ok ? "Installed" : "Could not install", path, (unsigned)len);
        return;
    }
    ESP_LOGW(TAG, "No phrase file for language '%s': the English phrases are used", code);
}

/* "[name]" is a line of the text (name: name_len characters, not NUL-terminated) */
static bool has_section(const char *text, const char *name_chars, size_t name_len)
{
    char name[48];
    if (name_len >= sizeof(name)) {
        return true;    // not a list name: nothing to add
    }
    memcpy(name, name_chars, name_len);
    name[name_len] = '\0';
    for (const char *p = text; (p = strstr(p, name)) != NULL; p += name_len) {
        bool line_start = (p >= text + 1 && p[-1] == '[') && (p == text + 1 || p[-2] == '\n');
        if (line_start && p[name_len] == ']') {
            return true;
        }
    }
    return false;
}

/*
 * Sections of the firmware's file missing in the one of SPIFFS (added by a newer firmware) are appended to it,
 * with their comments; what is there (possibly edited) is kept
 */
static void add_missing_sections(const char *code, const char *path, const char *text)
{
    for (size_t i = 0; i < sizeof(s_defaults) / sizeof(s_defaults[0]); i++) {
        if (strcasecmp(code, s_defaults[i].code) != 0) {
            continue;
        }
        const char *def = s_defaults[i].start;
        const char *def_end = s_defaults[i].end - 1;    // without the added NUL
        FILE *f = NULL;
        for (const char *h = def; h && h < def_end; h = strstr(h + 1, "\n[s_")) {
            const char *header = (*h == '\n') ? h + 1 : h;
            if (header[0] != '[') {
                continue;
            }
            const char *close = strchr(header, ']');
            const char *next = strstr(header, "\n[s_");
            const char *block_end = next ? next + 1 : def_end;
            if (!close || close > block_end || has_section(text, header + 1, (size_t)(close - header - 1))) {
                continue;
            }
            if (!f && !(f = fopen(path, "a"))) {
                return;
            }
            fputs("\n", f);
            fwrite(header, 1, (size_t)(block_end - header), f);
            ESP_LOGI(TAG, "%s: section %.*s added", path, (int)(close - header + 1), header);
        }
        if (f) {
            fclose(f);
        }
        return;
    }
}

/* A line of [start, end), its blanks and '\r' trimmed */
static void trim_line(const char **start, const char **end)
{
    while (*start < *end && (**start == ' ' || **start == '\t')) {
        (*start)++;
    }
    while (*end > *start && ((*end)[-1] == '\r' || (*end)[-1] == ' ' || (*end)[-1] == '\t')) {
        (*end)--;
    }
}

/* The block [block, block_end) has this pattern line */
static bool block_has_line(const char *block, const char *block_end, const char *line, size_t len)
{
    for (const char *p = block; p < block_end;) {
        const char *e = memchr(p, '\n', (size_t)(block_end - p));
        const char *next = e ? e + 1 : block_end;
        e = e ? e : block_end;
        const char *a = p;
        trim_line(&a, &e);
        if ((size_t)(e - a) == len && memcmp(a, line, len) == 0) {
            return true;
        }
        p = next;
    }
    return false;
}

/* The body of the section "[name]" of a text (after its header line, up to the next section), NULL if none */
static const char *section_body(const char *text, const char *text_end, const char *name, size_t name_len,
                                const char **body_end)
{
    for (const char *p = text; p < text_end;) {
        const char *e = memchr(p, '\n', (size_t)(text_end - p));
        const char *next = e ? e + 1 : text_end;
        if (strncmp(p, "[s_", 3) == 0 && (size_t)(text_end - p) > name_len + 1 && memcmp(p + 1, name, name_len) == 0 &&
                p[name_len + 1] == ']') {
            const char *end = strstr(next, "\n[s_");
            *body_end = (end && end < text_end) ? end + 1 : text_end;
            return next;
        }
        p = next;
    }
    return NULL;
}

/*
 * The patterns of the firmware's file missing in a section of the file of SPIFFS (a section improved by a newer
 * firmware) are added at its end: the lines there (possibly edited) are kept, and an emptied section (the command
 * disabled) stays empty. A pattern removed from a non-empty section comes back.
 */
static void add_missing_patterns(const char *code, const char *path)
{
    const char *def = NULL, *def_end = NULL;
    for (size_t i = 0; i < sizeof(s_defaults) / sizeof(s_defaults[0]); i++) {
        if (strcasecmp(code, s_defaults[i].code) == 0) {
            def = s_defaults[i].start;
            def_end = s_defaults[i].end - 1;    // without the added NUL
        }
    }
    char *text = def ? read_file(path) : NULL;
    char *out = text ? heap_caps_malloc(PHRASES_FILE_MAX, MALLOC_CAP_SPIRAM) : NULL;
    if (!out) {
        free(text);
        return;
    }
    const char *text_end = text + strlen(text);
    const char *eol = strchr(text, '\r') ? "\r\n" : "\n";
    size_t n = 0;
    int added = 0;
    bool full = false;
    for (const char *p = text; p < text_end;) {
        const char *e = memchr(p, '\n', (size_t)(text_end - p));
        const char *next = e ? e + 1 : text_end;
        // A section header ("[s_..."), not a pattern starting with an optional part ("[la ](radio|station)")
        const char *close = (strncmp(p, "[s_", 3) == 0) ? memchr(p, ']', (size_t)(next - p)) : NULL;
        if (!close) {
            // A line outside a section header: as it is
            if (n + (size_t)(next - p) < PHRASES_FILE_MAX) {
                memcpy(out + n, p, (size_t)(next - p));
                n += (size_t)(next - p);
            }
            p = next;
            continue;
        }
        // A section: its header and body, the missing patterns after its last non-blank line
        const char *name = p + 1;
        size_t name_len = (size_t)(close - name);
        const char *body_end = strstr(next, "\n[s_");
        body_end = body_end ? body_end + 1 : text_end;
        const char *last = next;        // end of the last non-blank line of the body
        bool has_pattern = false;
        for (const char *q = next; q < body_end;) {
            const char *qe = memchr(q, '\n', (size_t)(body_end - q));
            const char *qn = qe ? qe + 1 : body_end;
            const char *a = q, *b = qe ? qe : body_end;
            trim_line(&a, &b);
            if (a < b) {
                last = qn;
                has_pattern |= (*a != '#');
            }
            q = qn;
        }
        if (n + (size_t)(last - p) < PHRASES_FILE_MAX) {
            memcpy(out + n, p, (size_t)(last - p));
            n += (size_t)(last - p);
        }
        if (last > p && last[-1] != '\n' && n + 2 < PHRASES_FILE_MAX) {
            n += (size_t)snprintf(out + n, PHRASES_FILE_MAX - n, "%s", eol);    // last line without its end
        }
        const char *def_body_end = NULL;
        const char *def_body = has_pattern ? section_body(def, def_end, name, name_len, &def_body_end) : NULL;
        for (const char *q = def_body; q && q < def_body_end;) {
            const char *qe = memchr(q, '\n', (size_t)(def_body_end - q));
            const char *qn = qe ? qe + 1 : def_body_end;
            const char *a = q, *b = qe ? qe : def_body_end;
            trim_line(&a, &b);
            if (a < b && *a != '#' && !block_has_line(next, body_end, a, (size_t)(b - a))) {
                if (n + (size_t)(b - a) + 3 >= PHRASES_FILE_MAX) {
                    full = true;
                } else {
                    memcpy(out + n, a, (size_t)(b - a));
                    n += (size_t)(b - a);
                    n += (size_t)snprintf(out + n, PHRASES_FILE_MAX - n, "%s", eol);
                    ESP_LOGI(TAG, "%s: [%.*s] %.*s added", path, (int)name_len, name, (int)(b - a), a);
                    added++;
                }
            }
            q = qn;
        }
        // The blank lines after the section
        if (n + (size_t)(body_end - last) < PHRASES_FILE_MAX) {
            memcpy(out + n, last, (size_t)(body_end - last));
            n += (size_t)(body_end - last);
        }
        p = body_end;
    }
    if (full) {
        ESP_LOGW(TAG, "%s: larger than %d bytes, some patterns of the firmware not added", path, PHRASES_FILE_MAX);
    }
    if (added) {
        FILE *f = fopen(path, "w");
        bool ok = f && fwrite(out, 1, n, f) == n;
        if (f) {
            ok &= (fclose(f) == 0);
        }
        ESP_LOGI(TAG, "%s: %d pattern(s) of the firmware added%s", path, added, ok ? "" : ", could not write");
    }
    free(out);
    free(text);
}

bool phrases_create(const char *code, const char *name)
{
    char path[48];
    snprintf(path, sizeof(path), PHRASES_FILE_FORMAT, code);
    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        return false;
    }
    for (size_t i = 0; i < sizeof(s_defaults) / sizeof(s_defaults[0]); i++) {
        if (strcasecmp(code, s_defaults[i].code) == 0) {
            return false;   // given with the firmware (installed when missing)
        }
    }
    f = fopen(path, "w");
    if (!f) {
        return false;
    }
    fprintf(f, "# %s (%s): new language, made from the English phrases. Translate the patterns below (keep the\n"
            "# [s_...] section names). Until then, they are the English ones.\n\n", name, code);
    fwrite(s_default_en_start, 1, (size_t)(s_default_en_end - s_default_en_start) - 1, f);
    fclose(f);
    ESP_LOGI(TAG, "%s made for %s", path, name);
    return true;
}

void phrases_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    s_lang_count = app_lang_count() < PHRASES_LANG_MAX ? app_lang_count() : PHRASES_LANG_MAX;
    for (size_t i = 0; i < s_lang_count; i++) {
        char path[48];
        snprintf(path, sizeof(path), PHRASES_FILE_FORMAT, app_lang_code(i));
        char *text = read_file(path);
        if (!text || is_blank_text(text)) {
            // Missing (first start) or emptied on the configuration drive: the version given with the firmware
            free(text);
            install_default(app_lang_code(i), path);
            text = read_file(path);
        } else {
            // Kept as it is (possibly edited); the lists added by a newer firmware are appended
            add_missing_sections(app_lang_code(i), path, text);
            add_missing_patterns(app_lang_code(i), path);
            free(text);
            text = read_file(path);
        }
        free(s_texts[i]);
        s_texts[i] = text;
        ESP_LOGI(TAG, "%s: %u bytes", path, text ? (unsigned)strlen(text) : 0);
    }
}

static int lang_index(const char *code)
{
    for (size_t i = 0; i < s_lang_count; i++) {
        if (strcasecmp(code, app_lang_code(i)) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/*
 * The patterns of a section ("[name]" up to the next "[...]" line), without comments and blank lines, into
 * out (allocated); *found when the section exists, even empty
 */
static int section_patterns(const char *text, const char *name, char **out, int max, bool *found)
{
    int n = 0;
    bool in_section = false;
    size_t name_len = strlen(name);
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        const char *next = end ? end + 1 : line + strlen(line);
        if (!end) {
            end = next;
        }
        // Without the blanks around it (and the '\r' of Windows line ends)
        while (line < end && (unsigned char)*line <= ' ') {
            line++;
        }
        while (end > line && (unsigned char)end[-1] <= ' ') {
            end--;
        }
        size_t len = (size_t)(end - line);
        if (len >= 2 && line[0] == '[' && end[-1] == ']' && line[1] == 's' && line[2] == '_') {
            in_section = (len == name_len + 2 && strncmp(line + 1, name, name_len) == 0);
            *found |= in_section;
        } else if (in_section && len > 0 && line[0] != '#' && n < max) {
            out[n] = ps_strndup(line, len);
            n += (out[n] != NULL);
        }
        line = next;
    }
    return n;
}

/* The phrases of the patterns, without duplicates, longest first; NULL if none */
static const char **make_phrases(char **patterns, int pattern_count)
{
    int count = 0;
    char **all = NULL;
    for (int p = 0; p < pattern_count; p++) {
        // Off the caller's stack, in PSRAM (internal RAM is scarce); used under s_lock
        static char **made = NULL;
        if (!made && !(made = heap_caps_calloc(PHRASES_PER_PATTERN_MAX, sizeof(char *), MALLOC_CAP_SPIRAM))) {
            break;
        }
        int n = phrases_expand(patterns[p], made, PHRASES_PER_PATTERN_MAX);
        char **grown = ps_realloc(all, (count + n + 1) * sizeof(char *));
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
        return NULL;
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
    return (const char **)all;
}

const char *const *phrases_of(phrase_list_t *list)
{
    static const char *s_empty[] = { NULL };
    int slot = PHRASES_LANG_MAX;
    if (!list->all_languages) {
        // The current language; a language without UI texts (not in file_lang.c) takes the English phrases
        slot = lang_index(app_lang_get()->code);
        if (slot < 0) {
            slot = lang_index(PHRASES_FALLBACK_LANG);
        }
        if (slot < 0) {
            return s_empty;
        }
    }
    if (list->phrases[slot]) {
        return list->phrases[slot];
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (!list->phrases[slot]) {
        static char **patterns = NULL;      // PSRAM (internal RAM is scarce)
        if (!patterns) {
            patterns = heap_caps_calloc(PATTERNS_PER_LIST_MAX, sizeof(char *), MALLOC_CAP_SPIRAM);
            assert(patterns);
        }
        int n = 0;
        bool found = false;
        if (list->all_languages) {
            for (size_t i = 0; i < s_lang_count; i++) {
                n += section_patterns(s_texts[i], list->name, patterns + n, PATTERNS_PER_LIST_MAX - n, &found);
            }
        } else {
            n = section_patterns(s_texts[slot], list->name, patterns, PATTERNS_PER_LIST_MAX, &found);
            int fallback = lang_index(PHRASES_FALLBACK_LANG);
            if (!found && fallback >= 0 && fallback != slot) {
                ESP_LOGW(TAG, "[%s] missing for '%s': English phrases used", list->name, app_lang_code(slot));
                n = section_patterns(s_texts[fallback], list->name, patterns, PATTERNS_PER_LIST_MAX, &found);
            }
        }
        if (!found) {
            ESP_LOGE(TAG, "[%s] found in no phrase file: this command is not recognised", list->name);
        }
        const char **made = make_phrases(patterns, n);
        int count = 0;
        for (const char **p = made; p && *p; p++) {
            count++;
        }
        ESP_LOGI(TAG, "[%s] %s: %d phrases from %d patterns", list->name,
                 list->all_languages ? "all languages" : app_lang_code(slot), count, n);
        for (int i = 0; i < n; i++) {
            free(patterns[i]);
        }
        list->phrases[slot] = made ? made : s_empty;
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    return list->phrases[slot];
}
