/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Games between BOX: the games, their phrases, the tags of their lines, what they used lately (see
 * game_internal.h).
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "file_lang.h"
#include "file_state.h"
#include "file_phrases.h"
#include "game.h"
#include "game_internal.h"

static const char *TAG = "game";

#define GAMES_FILE          "/spiffs/games.ini"
#define GAMES_FILE_MARK     "# GAMES.INI v2"
/* GAMES.INI of an older firmware (a whole copy of its games), set aside */
#define GAMES_OLD_FILE      "/spiffs/games_old.ini"
#define GAMES_MAX           12
/* What the games used lately (the answers of the riddles, the themes): not taken again */
#define GAMES_USED_FILE     STATE_DIR "/games_used.txt"
#define GAMES_USED_KEPT     60

/* The firmware's games, built in (main/games/games.ini, embedded NUL-terminated) */
extern const char s_games_default_start[] asm("_binary_games_ini_start");
/* GAMES.INI as installed: the help on the keys, no game (the user's games and changes go there) */
extern const char s_games_user_start[] asm("_binary_games_user_ini_start");

static game_def_t *s_games = NULL;      /* PSRAM */
/*
 * The games read by the main task (a claim received) and by the SR handler task (the phrase heard) at the same
 * time: the phrase was compared with a list being read again, empty, and went to the model
 */
static SemaphoreHandle_t s_games_lock = NULL;
static int s_game_count = 0;
static char s_games_lang[8] = "";

/*
 * The keys of a game, besides name_<lang> and phrases_<lang>: read, merged and freed by this table (a new key: a
 * field of game_def_t and a line here)
 */
typedef enum {
    KEY_TEXT,               /* char *, allocated */
    KEY_CHARS,              /* char[], its size */
    KEY_INT,
    KEY_FLAG,               /* bool: the value is the word given ("yes", "no", "first") */
} key_kind_t;

typedef struct {
    const char *name;
    key_kind_t kind;
    size_t offset;
    size_t size;
    const char *flag;
} game_key_t;

#define KEY_T(key, field)       { key, KEY_TEXT, offsetof(game_def_t, field), 0, NULL }
#define KEY_C(key, field)       { key, KEY_CHARS, offsetof(game_def_t, field), sizeof(((game_def_t *)0)->field), NULL }
#define KEY_I(key, field)       { key, KEY_INT, offsetof(game_def_t, field), 0, NULL }
#define KEY_F(key, field, word) { key, KEY_FLAG, offsetof(game_def_t, field), 0, word }

static const game_key_t s_keys[] = {
    KEY_T("first", first),
    KEY_T("rules", rules),
    KEY_T("last", last),
    KEY_F("character", plain, "no"),
    KEY_T("characters", characters),
    KEY_I("sentences", sentences),
    KEY_I("lines", lines),
    KEY_F("reasoning", reasoning, "yes"),
    KEY_T("themes", themes),
    KEY_T("theme_rule", theme_rule),
    KEY_C("theme_for", theme_for),
    KEY_C("document", document),
    KEY_T("document_rule", document_rule),
    KEY_C("short_after", short_after),
    KEY_C("secret", secret),
    KEY_C("secret_with", secret_with),
    KEY_C("next", next),
    KEY_C("turns", turns),
    KEY_I("blunder", blunder),
    KEY_C("blunder_after", blunder_after),
    KEY_T("blunder_rule", blunder_rule),
};

static bool key_set(game_def_t *g, const char *key, const char *value)
{
    for (size_t i = 0; i < sizeof(s_keys) / sizeof(s_keys[0]); i++) {
        const game_key_t *k = &s_keys[i];
        if (strcmp(key, k->name) != 0) {
            continue;
        }
        char *field = (char *)g + k->offset;
        switch (k->kind) {
        case KEY_TEXT:
            free(*(char **)field);
            *(char **)field = strdup(value);
            break;
        case KEY_CHARS:
            strlcpy(field, value, k->size);
            break;
        case KEY_INT:
            *(int *)field = atoi(value);
            break;
        case KEY_FLAG:
            *(bool *)field = strcasecmp(value, k->flag) == 0;
            break;
        }
        return true;
    }
    return false;
}

static void trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
}

/*
 * The games: the firmware's (main/games/games.ini, built in), then GAMES.INI of the configuration drive, which holds
 * only the user's: a section of a firmware game changes the keys written in it, another one adds a game. No copy of
 * the firmware's games to update: a change of their rules is in the firmware only.
 */
static void games_install(void)
{
    FILE *f = fopen(GAMES_FILE, "r");
    char head[32] = "";
    long size = 0;
    if (f) {
        size_t n = fread(head, 1, sizeof(head) - 1, f);
        head[n] = '\0';
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fclose(f);
    }
    if (size > 0 && strncmp(head, GAMES_FILE_MARK, strlen(GAMES_FILE_MARK)) == 0) {
        return;
    }
    if (size > 0) {
        // A whole copy of the games by an older firmware: it would override the firmware's rules; set aside
        remove(GAMES_OLD_FILE);
        rename(GAMES_FILE, GAMES_OLD_FILE);
        ESP_LOGW(TAG, "%s of an older firmware kept as %s", GAMES_FILE, GAMES_OLD_FILE);
    }
    f = fopen(GAMES_FILE, "w");
    bool ok = f && fputs(s_games_user_start, f) >= 0;
    if (f) {
        ok &= fclose(f) == 0;
    }
    ESP_LOGI(TAG, "%s %s", ok ? "Installed" : "Could not install", GAMES_FILE);
}

static void games_free(void)
{
    for (int i = 0; i < s_game_count; i++) {
        for (size_t k = 0; k < sizeof(s_keys) / sizeof(s_keys[0]); k++) {
            if (s_keys[k].kind == KEY_TEXT) {
                char **text = (char **)((char *)&s_games[i] + s_keys[k].offset);
                free(*text);
                *text = NULL;
            }
        }
    }
    s_game_count = 0;
}

static void games_load_locked(void);

/* Read again for the language in use (its names and phrases, else the English ones) */
void games_load(void)
{
    if (s_games_lock) {
        xSemaphoreTake(s_games_lock, portMAX_DELAY);
    }
    games_load_locked();
    if (s_games_lock) {
        xSemaphoreGive(s_games_lock);
    }
}

/* The sections of a text (the firmware's games, then GAMES.INI): a section already read gets the keys written */
static void games_parse(const char *text, const char *key_name, const char *key_phrases)
{
    char *line = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    game_def_t *g = NULL;
    for (const char *p = text; line && p && *p; ) {
        size_t n = strcspn(p, "\n");
        snprintf(line, 2048, "%.*s", (int)(n < 2047 ? n : 2047), p);
        p += n + (p[n] == '\n');
        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            g = NULL;
            if (!end) {
                continue;
            }
            *end = '\0';
            for (int i = 0; i < s_game_count && !g; i++) {
                if (strcmp(s_games[i].id, line + 1) == 0) {
                    g = &s_games[i];        // a firmware game changed by GAMES.INI
                }
            }
            if (!g && s_game_count < GAMES_MAX) {
                g = &s_games[s_game_count++];
                memset(g, 0, sizeof(*g));
                strlcpy(g->id, line + 1, sizeof(g->id));
                strlcpy(g->name, g->id, sizeof(g->name));
            }
            continue;
        }
        char *eq = strchr(line, '=');
        if (!g || !eq) {
            continue;
        }
        *eq = '\0';
        char *key = line, *value = eq + 1;
        trim(key);
        trim(value);
        if (strcmp(key, key_name) == 0 || (strcmp(key, "name_en") == 0 && !g->name_own)) {
            strlcpy(g->name, value, sizeof(g->name));
            g->name_own |= strcmp(key, key_name) == 0;
        } else if (strcmp(key, key_phrases) == 0 || (strcmp(key, "phrases_en") == 0 && !g->phrases_own)) {
            strlcpy(g->phrases, value, sizeof(g->phrases));
            g->phrases_own |= strcmp(key, key_phrases) == 0;
        } else if (!key_set(g, key, value) && strncmp(key, "name_", 5) != 0 && strncmp(key, "phrases_", 8) != 0) {
            ESP_LOGW(TAG, "[%s]: unknown key \"%s\" left out", g->id, key);
        }
    }
    free(line);
}

static void games_load_locked(void)
{
    const char *lang = app_lang_get()->code;
    if (s_games && strcmp(s_games_lang, lang) == 0) {
        return;
    }
    if (!s_games) {
        s_games = heap_caps_calloc(GAMES_MAX, sizeof(game_def_t), MALLOC_CAP_SPIRAM);
    }
    if (!s_games) {
        return;
    }
    games_free();
    strlcpy(s_games_lang, lang, sizeof(s_games_lang));
    char key_name[24], key_phrases[24];
    snprintf(key_name, sizeof(key_name), "name_%s", lang);
    snprintf(key_phrases, sizeof(key_phrases), "phrases_%s", lang);
    for (char *c = key_name; *c; c++) {
        *c = (char)tolower((unsigned char)*c);
    }
    for (char *c = key_phrases; *c; c++) {
        *c = (char)tolower((unsigned char)*c);
    }
    games_parse(s_games_default_start, key_name, key_phrases);
    int builtin = s_game_count;
    // GAMES.INI: the user's games and changes
    FILE *f = fopen(GAMES_FILE, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *text = size > 0 ? heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM) : NULL;
        size_t got = text ? fread(text, 1, size, f) : 0;
        fclose(f);
        if (text) {
            text[got] = '\0';
            games_parse(text, key_name, key_phrases);
            free(text);
        }
    }
    ESP_LOGI(TAG, "%d game(s): %d of the firmware, %d added by %s", s_game_count, builtin, s_game_count - builtin,
             GAMES_FILE);
}

void games_init(void)
{
    s_games_lock = xSemaphoreCreateMutex();
    games_install();
}

char *game_help(void)
{
    // As loaded (at start-up, on a change of language): no file read from the model's task
    char *out = NULL;
    size_t size = 0;
    FILE *f = open_memstream(&out, &size);
    if (!f) {
        return NULL;
    }
    for (int i = 0; i < s_game_count; i++) {
        fprintf(f, "%s: %s\n", s_games[i].name, s_games[i].phrases);
    }
    fclose(f);
    return out;
}

const game_def_t *game_def(int game)
{
    return &s_games[game];
}

int game_find(const char *id)
{
    for (int i = 0; i < s_game_count; i++) {
        if (strcmp(s_games[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

/* A phrase to compare: lower case, without accents (UTF-8 Latin letters) nor punctuation, single spaces */
static void fold(const char *in, char *out, size_t size)
{
    static const char from[] = "àáâãäåçèéêëìíîïñòóôõöùúûüýÿ";
    static const char to[] = "aaaaaaceeeeiiiinooooouuuuyy";
    size_t n = 0;
    bool space = false;
    for (const unsigned char *p = (const unsigned char *)in; *p && n + 2 < size; p++) {
        char c = 0;
        if (p[0] == 0xC3 && p[1]) {
            unsigned char second = p[1] | 0x20;
            for (size_t i = 0; i + 1 < sizeof(from) - 1; i += 2) {
                if ((unsigned char)from[i + 1] == second) {
                    c = to[i / 2];
                    break;
                }
            }
            p++;
            if (!c) {
                continue;
            }
        } else if (isalnum(*p)) {
            c = (char)tolower(*p);
        } else if (*p >= 0x80) {
            continue;
        }
        if (!c) {
            space = n > 0;      // punctuation, a dash, a blank: one space
            continue;
        }
        if (space) {
            out[n++] = ' ';
            space = false;
        }
        out[n++] = c;
    }
    out[n] = '\0';
}

/*
 * The game whose phrase begins what was said, -1 if none. The phrases of GAMES.INI are separated by "|" and written
 * as the voice commands' patterns ("invent(ez|er|ons) [quelque chose ]ensemble", file_phrases.h): the transcription
 * writes the verb or the small words its own way
 */
static int game_of(const char *text)
{
    char said[200];
    fold(text, said, sizeof(said));
    for (int i = 0; i < s_game_count; i++) {
        const char *p = s_games[i].phrases;
        while (*p) {
            // One pattern: up to a "|" outside brackets
            const char *end = p;
            for (int depth = 0; *end && (*end != '|' || depth > 0); end++) {
                depth += (*end == '(' || *end == '[') - (*end == ')' || *end == ']');
            }
            char pattern[160];
            snprintf(pattern, sizeof(pattern), "%.*s", (int)(end - p), p);
            trim(pattern);
            for (size_t n = strlen(pattern); n && pattern[n - 1] == ' '; n--) {
                pattern[n - 1] = '\0';
            }
            char *phrases[48];
            int count = pattern[0] ? phrases_expand(pattern, phrases, 48) : 0;
            bool found = false;
            for (int k = 0; k < count; k++) {
                char phrase[100];
                fold(phrases[k], phrase, sizeof(phrase));
                size_t len = strlen(phrase);
                found |= len && strncmp(said, phrase, len) == 0 && (said[len] == '\0' || said[len] == ' ');
                free(phrases[k]);
            }
            if (found) {
                return i;
            }
            p = *end ? end + 1 : end;
        }
    }
    return -1;
}

int game_phrase(const char *text)
{
    if (!s_games_lock) {
        return -1;
    }
    xSemaphoreTake(s_games_lock, portMAX_DELAY);
    games_load_locked();
    int game = game_of(text);
    xSemaphoreGive(s_games_lock);
    return game;
}

/* ---------- lists and tags ---------- */

int game_list_count(const char *list)
{
    if (!list || !list[0]) {
        return 0;
    }
    int count = 1;
    for (const char *p = list; *p; p++) {
        count += *p == '|';
    }
    return count;
}

bool game_list_item(const char *list, int index, char *out, size_t size)
{
    out[0] = '\0';
    if (index < 0 || index >= game_list_count(list)) {
        return false;
    }
    const char *p = list;
    for (int i = 0; i < index; i++) {
        p = strchr(p, '|') + 1;
    }
    p += strspn(p, " ");
    size_t n = strcspn(p, "|");
    while (n && p[n - 1] == ' ') {
        n--;
    }
    snprintf(out, size, "%.*s", (int)n, p);
    return true;
}

/*
 * The tag a line may start with ("RIGHT: Correct !"), asked by the rules: an upper-case word and a colon. Returns
 * the text after it (and its spaces), the text itself without a tag; its length in *tag_len (0: none)
 */
const char *game_tag_skip(const char *text, size_t *tag_len)
{
    const char *p = text;
    while (*p == ' ' || *p == '\n') {
        p++;
    }
    const char *word = p;
    while (*p >= 'A' && *p <= 'Z') {
        p++;
    }
    size_t len = p - word;
    while (*p == ' ') {
        p++;
    }
    if (len < 2 || len > 12 || *p != ':') {
        *tag_len = 0;
        return text;
    }
    *tag_len = len;
    p++;
    while (*p == ' ' || *p == '\n') {
        p++;
    }
    return p;
}

void game_tag_of(const char *text, char *tag, size_t size)
{
    size_t len = 0;
    game_tag_skip(text, &len);
    const char *start = text + strspn(text, " \n");
    snprintf(tag, size, "%.*s", (int)(len < size ? len : size - 1), start);
}

/* A word of a list of tags ("RIDDLE WRONG") */
bool game_tag_in(const char *list, size_t list_len, const char *tag)
{
    size_t tag_len = strlen(tag);
    for (const char *p = list; p < list + list_len; ) {
        p += strspn(p, " ,");
        size_t n = strcspn(p, " ,;");
        if (p + n > list + list_len) {
            n = list + list_len - p;
        }
        if (n && n == tag_len && strncmp(p, tag, n) == 0) {
            return true;
        }
        p += n ? n : 1;
    }
    return false;
}

/* The right side of the rule of this tag in "A>B C; D>E" (copied in out; false: no rule) */
bool game_rule_of(const char *rules, const char *tag, char *out, size_t size)
{
    for (const char *r = rules; tag[0] && *r; ) {
        r += strspn(r, " ;");
        size_t len = strcspn(r, ";");
        const char *gt = memchr(r, '>', len);
        if (gt) {
            size_t left = gt - r;
            while (left && r[left - 1] == ' ') {
                left--;
            }
            if (left == strlen(tag) && strncmp(r, tag, left) == 0) {
                const char *right = gt + 1 + strspn(gt + 1, " ");
                size_t n = r + len - right;
                while (n && right[n - 1] == ' ') {
                    n--;
                }
                snprintf(out, size, "%.*s", (int)n, right);
                return true;
            }
        }
        r += len;
    }
    return false;
}

/* ---------- what the games used lately ---------- */

/* The items of this game used lately, oldest first, separated by "\n" (allocated; NULL: none) */
char *game_used_read(const char *game)
{
    FILE *f = fopen(GAMES_USED_FILE, "r");
    if (!f) {
        return NULL;
    }
    char *out = NULL;
    size_t size = 0;
    FILE *mem = open_memstream(&out, &size);
    char line[160];
    size_t id_len = strlen(game);
    while (mem && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, game, id_len) == 0 && line[id_len] == '\t' && line[id_len + 1]) {
            fprintf(mem, "%s\n", line + id_len + 1);
        }
    }
    fclose(f);
    if (mem) {
        fclose(mem);
    }
    if (out && !out[0]) {
        free(out);
        out = NULL;
    }
    return out;
}

/* One more item used by this game: the file keeps the last GAMES_USED_KEPT lines */
void game_used_add(const char *game, const char *item)
{
    char *old = NULL;
    size_t size = 0;
    FILE *f = fopen(GAMES_USED_FILE, "r");
    if (f) {
        FILE *mem = open_memstream(&old, &size);
        char line[160];
        while (mem && fgets(line, sizeof(line), f)) {
            fputs(line, mem);
        }
        if (mem) {
            fclose(mem);
        }
        fclose(f);
    }
    int lines = 0;
    for (const char *p = old; p && *p; p++) {
        lines += *p == '\n';
    }
    const char *from = old;
    for (; from && lines >= GAMES_USED_KEPT; lines--) {
        const char *nl = strchr(from, '\n');
        from = nl ? nl + 1 : from + strlen(from);
    }
    f = fopen(GAMES_USED_FILE, "w");
    if (f) {
        if (from) {
            fputs(from, f);
        }
        fprintf(f, "%s\t%.120s\n", game, item);
        fclose(f);
    }
    free(old);
}

/* The item is in the list ("a\nb\n"), case ignored */
static bool used_has(const char *used, const char *item)
{
    size_t len = strlen(item);
    for (const char *p = used; p && *p; ) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n == len && strncasecmp(p, item, n) == 0) {
            return true;
        }
        p += n + (nl != NULL);
    }
    return false;
}

/* A theme of the game drawn, not one used lately if possible */
void game_theme_draw(const game_def_t *g, const char *used, char *theme, size_t size)
{
    theme[0] = '\0';
    int count = game_list_count(g->themes);
    for (int tries = 0; count && tries < 12; tries++) {
        game_list_item(g->themes, (int)(esp_random() % count), theme, size);
        if (!used_has(used, theme)) {
            return;
        }
    }
}

