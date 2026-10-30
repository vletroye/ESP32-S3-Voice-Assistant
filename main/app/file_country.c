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
#include <strings.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "file_lang.h"
#include "file_memory.h"
#include "file_country.h"

#define COUNTRY_FILE_MAX        16384   /* as on the configuration drive */
#define COUNTRY_COLUMNS_MAX     16
#define COUNTRY_FALLBACK_LANG   "en"
#define COUNTRY_NAME_MAX        96

static const char *TAG = "country";

/* The file given with the firmware (main/lang/country.txt, embedded NUL-terminated), installed when missing or emptied */
extern const char s_default_start[] asm("_binary_country_txt_start");
extern const char s_default_end[] asm("_binary_country_txt_end");

typedef struct {
    char code[3];
    const char *names[COUNTRY_COLUMNS_MAX];     /* "Netherlands/Holland", NULL when empty */
} country_t;

/* A file read: the names point into its text (PSRAM) */
typedef struct {
    char *text;
    char columns[COUNTRY_COLUMNS_MAX][LANG_CODE_SIZE];   /* "en", "fr"... */
    size_t column_count;
    country_t *rows;
    size_t count;
} country_table_t;

static country_table_t s_table;

/* ---------- reading ---------- */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
    return s;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }
    char *text = heap_caps_malloc(COUNTRY_FILE_MAX, MALLOC_CAP_SPIRAM);
    size_t n = text ? fread(text, 1, COUNTRY_FILE_MAX - 1, f) : 0;
    fclose(f);
    if (text) {
        text[n] = '\0';
    }
    return text;
}

static bool is_blank(const char *text)
{
    for (; text && *text; text++) {
        if ((unsigned char)*text > ' ') {
            return false;
        }
    }
    return true;
}

static void table_free(country_table_t *t)
{
    free(t->text);
    free(t->rows);
    memset(t, 0, sizeof(*t));
}

/* The header "EN, FR, DE" and the lines "BE: Belgium, Belgique, ..." of a text (modified in place and kept) */
static void parse(char *text, country_table_t *t)
{
    memset(t, 0, sizeof(*t));
    t->text = text;
    size_t lines = 1;
    for (const char *p = text; *p; p++) {
        lines += (*p == '\n');
    }
    t->rows = heap_caps_calloc(lines, sizeof(country_t), MALLOC_CAP_SPIRAM);
    bool header = false;
    for (char *line = text; t->rows && line && *line;) {
        char *end = strchr(line, '\n');
        if (end) {
            *end = '\0';
        }
        char *next = end ? end + 1 : NULL;
        char *s = trim(line);
        char *colon = strchr(s, ':');
        if (!s[0] || s[0] == '#') {
            // comment or blank line
        } else if (!header) {
            header = true;
            for (char *item = s; item;) {
                char *comma = strchr(item, ',');
                if (comma) {
                    *comma = '\0';
                }
                if (t->column_count < COUNTRY_COLUMNS_MAX) {
                    char *code = trim(item);
                    for (char *c = code; *c; c++) {
                        *c = tolower((unsigned char)*c);
                    }
                    strlcpy(t->columns[t->column_count++], code, LANG_CODE_SIZE);
                }
                item = comma ? comma + 1 : NULL;
            }
        } else if (colon && colon - s == 2) {
            *colon = '\0';
            country_t *c = &t->rows[t->count++];
            c->code[0] = toupper((unsigned char)s[0]);
            c->code[1] = toupper((unsigned char)s[1]);
            size_t i = 0;
            for (char *name = colon + 1; name && i < COUNTRY_COLUMNS_MAX; i++) {
                char *comma = strchr(name, ',');
                if (comma) {
                    *comma = '\0';
                }
                name = trim(name);
                c->names[i] = name[0] ? name : NULL;
                name = comma ? comma + 1 : NULL;
            }
        } else {
            ESP_LOGW(TAG, "Line left out (\"XX: names\" expected): %s", s);
        }
        line = next;
    }
}

static int column_of(const country_table_t *t, const char *code)
{
    for (size_t i = 0; i < t->column_count; i++) {
        if (strcasecmp(t->columns[i], code) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static const country_t *row_of(const country_table_t *t, const char *code)
{
    for (size_t i = 0; i < t->count; i++) {
        if (strcasecmp(t->rows[i].code, code) == 0) {
            return &t->rows[i];
        }
    }
    return NULL;
}

/* ---------- writing ---------- */

static void install_default(void)
{
    size_t len = (size_t)(s_default_end - s_default_start) - 1;     // without the added NUL
    FILE *f = fopen(COUNTRY_FILE, "w");
    bool ok = f && fwrite(s_default_start, 1, len, f) == len;
    if (f) {
        ok &= (fclose(f) == 0);
    }
    ESP_LOGI(TAG, "%s %s (%u bytes)", ok ? "Installed" : "Could not install", COUNTRY_FILE, (unsigned)len);
}

/*
 * The file written again with the columns `codes` added: at the end of the header, and at the end of each country
 * (after the columns it lacks), its names in `from` (a previous file) or empty
 */
static bool add_columns(char codes[][LANG_CODE_SIZE], size_t n, const country_table_t *from)
{
    char *text = read_file(COUNTRY_FILE);
    FILE *f = text ? fopen(COUNTRY_FILE ".tmp", "w") : NULL;
    if (!f) {
        free(text);
        return false;
    }
    bool header = false;
    for (char *line = text; line && *line;) {
        char *end = strchr(line, '\n');
        if (end) {
            *end = '\0';
        }
        char *next = end ? end + 1 : NULL;
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
            line[--len] = '\0';
        }
        const char *s = line + strspn(line, " \t");
        const char *colon = strchr(s, ':');
        fputs(line, f);
        if (!s[0] || s[0] == '#') {
            // comment or blank line, as it is
        } else if (!header) {
            header = true;
            for (size_t i = 0; i < n; i++) {
                fputs(", ", f);
                for (const char *c = codes[i]; *c; c++) {
                    fputc(toupper((unsigned char)*c), f);
                }
            }
        } else if (colon && colon - s == 2) {
            size_t columns = 1;
            for (const char *p = colon; *p; p++) {
                columns += (*p == ',');
            }
            for (; columns < s_table.column_count; columns++) {
                fputs(",", f);
            }
            char code[3] = { s[0], s[1], '\0' };
            const country_t *row = from ? row_of(from, code) : NULL;
            for (size_t i = 0; i < n; i++) {
                int column = from ? column_of(from, codes[i]) : -1;
                const char *names = (row && column >= 0) ? row->names[column] : NULL;
                fprintf(f, ", %s", names ? names : "");
            }
        }
        fputc('\n', f);
        line = next;
    }
    free(text);
    bool ok = (fclose(f) == 0);
    remove(COUNTRY_FILE);
    ok &= (rename(COUNTRY_FILE ".tmp", COUNTRY_FILE) == 0);
    return ok;
}

static void reload(void)
{
    table_free(&s_table);
    char *text = read_file(COUNTRY_FILE);
    if (text) {
        parse(text, &s_table);
    }
}

/* ---------- interface ---------- */

void country_init(void)
{
    char *text = read_file(COUNTRY_FILE);
    if (is_blank(text)) {
        free(text);
        install_default();
    } else {
        free(text);
    }
    reload();

    // A soft reset installed the firmware's file: the columns of the languages added (their translation) taken
    // from the previous one
    char *old = read_file(COUNTRY_OLD_FILE);
    if (old) {
        country_table_t previous;
        parse(old, &previous);
        char codes[COUNTRY_COLUMNS_MAX][LANG_CODE_SIZE];
        size_t n = 0;
        for (size_t i = 0; i < previous.column_count; i++) {
            if (column_of(&s_table, previous.columns[i]) < 0 && s_table.column_count + n < COUNTRY_COLUMNS_MAX) {
                strlcpy(codes[n++], previous.columns[i], LANG_CODE_SIZE);
            }
        }
        if (n) {
            ESP_LOGI(TAG, "%u column(s) kept from the previous file", (unsigned)n);
            add_columns(codes, n, &previous);
        }
        table_free(&previous);
        remove(COUNTRY_OLD_FILE);
        reload();
    }

    // A language added to Languages: an empty column (its names in English until translated)
    char codes[COUNTRY_COLUMNS_MAX][LANG_CODE_SIZE];
    size_t n = 0;
    for (size_t i = 0; i < app_lang_count(); i++) {
        if (column_of(&s_table, app_lang_code(i)) < 0 && s_table.column_count + n < COUNTRY_COLUMNS_MAX) {
            strlcpy(codes[n++], app_lang_code(i), LANG_CODE_SIZE);
        }
    }
    if (n) {
        ESP_LOGI(TAG, "%u column(s) added for the languages offered", (unsigned)n);
        add_columns(codes, n, NULL);
        reload();
    }
    ESP_LOGI(TAG, "%u countries, %u languages", (unsigned)s_table.count, (unsigned)s_table.column_count);
}

/* The word appears as a whole word in the normalized text */
static bool has_word(const char *norm, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = strstr(norm, word); n && p; p = strstr(p + 1, word)) {
        if ((p == norm || p[-1] == ' ') && (p[n] == '\0' || p[n] == ' ')) {
            return true;
        }
    }
    return false;
}

/* The longest of the names "a/b/c" in the sentence: its length, 0 if none */
static size_t longest_name(const char *names, const char *norm)
{
    char copy[COUNTRY_NAME_MAX * 4];
    strlcpy(copy, names ? names : "", sizeof(copy));
    size_t best = 0;
    for (char *name = copy; name && *name;) {
        char *slash = strchr(name, '/');
        if (slash) {
            *slash = '\0';
        }
        char word[COUNTRY_NAME_MAX];
        app_text_normalize(name, word, sizeof(word));
        if (strlen(word) > best && has_word(norm, word)) {
            best = strlen(word);
        }
        name = slash ? slash + 1 : NULL;
    }
    return best;
}

bool country_find(const char *norm, char *code, size_t size)
{
    int columns[2] = { column_of(&s_table, app_lang_get()->code), column_of(&s_table, COUNTRY_FALLBACK_LANG) };
    const country_t *found = NULL;
    size_t best = 0;
    for (size_t i = 0; i < s_table.count; i++) {
        const country_t *c = &s_table.rows[i];
        // "BE" alone
        char lower[3] = { tolower((unsigned char)c->code[0]), tolower((unsigned char)c->code[1]), '\0' };
        if (strcmp(norm, lower) == 0) {
            found = c;
            break;
        }
        for (size_t k = 0; k < 2; k++) {
            if (columns[k] < 0 || (k == 1 && columns[1] == columns[0])) {
                continue;
            }
            size_t len = longest_name(c->names[columns[k]], norm);
            if (len > best) {
                best = len;
                found = c;
            }
        }
    }
    if (found) {
        strlcpy(code, found->code, size);
    }
    return found != NULL;
}

void country_name(const char *code, char *out, size_t size)
{
    const country_t *c = row_of(&s_table, code);
    int columns[2] = { column_of(&s_table, app_lang_get()->code), column_of(&s_table, COUNTRY_FALLBACK_LANG) };
    const char *names = NULL;
    for (size_t k = 0; c && !names && k < 2; k++) {
        names = columns[k] >= 0 ? c->names[columns[k]] : NULL;
    }
    if (!names) {
        strlcpy(out, code, size);
        return;
    }
    // The first name: "Netherlands" of "Netherlands/Holland"
    size_t len = strcspn(names, "/");
    while (len && names[len - 1] == ' ') {
        len--;
    }
    snprintf(out, size, "%.*s", (int)len, names);
}
