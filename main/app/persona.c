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
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "persona.h"

#define PERSONA_FILE_FORMAT     "/spiffs/persona_%s.txt"
#define PERSONA_FILE_MAX        8192    /* as on the configuration drive */
#define PERSONA_FALLBACK_LANG   "en"

static const char *TAG = "persona";

/* Files given with the firmware (main/persona/, embedded NUL-terminated), installed when missing or emptied */
extern const char s_default_fr_start[] asm("_binary_persona_fr_txt_start");
extern const char s_default_fr_end[] asm("_binary_persona_fr_txt_end");
extern const char s_default_en_start[] asm("_binary_persona_en_txt_start");
extern const char s_default_en_end[] asm("_binary_persona_en_txt_end");

static const struct {
    const char *code;
    const char *start;
    const char *end;
} s_defaults[] = {
    { "fr", s_default_fr_start, s_default_fr_end },
    { "en", s_default_en_start, s_default_en_end },
};

/* Names of the sections, French and English (case ignored) */
static const char *const s_names[PERSONA_SECTIONS][3] = {
    [PERSONA_IDENTITY] = { "identité", "identite", "identity" },
    [PERSONA_PERSONALITY] = { "personnalité", "personnalite", "personality" },
    [PERSONA_INSTRUCTIONS] = { "instructions", "instructions", "instructions" },
    [PERSONA_VOICE] = { "voix", "voice", "voice" },
};

/* The sections of the language loaded (PSRAM) */
static char *s_sections[PERSONA_SECTIONS];

static const char *default_text(const char *code)
{
    for (size_t i = 0; i < sizeof(s_defaults) / sizeof(s_defaults[0]); i++) {
        if (strcasecmp(code, s_defaults[i].code) == 0) {
            return s_defaults[i].start;
        }
    }
    return NULL;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }
    char *text = heap_caps_malloc(PERSONA_FILE_MAX, MALLOC_CAP_SPIRAM);
    size_t n = text ? fread(text, 1, PERSONA_FILE_MAX - 1, f) : 0;
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

static int section_of(const char *name, size_t len)
{
    for (int s = 0; s < PERSONA_SECTIONS; s++) {
        for (int n = 0; n < 3; n++) {
            if (strlen(s_names[s][n]) == len && strncasecmp(name, s_names[s][n], len) == 0) {
                return s;
            }
        }
    }
    return -1;
}

/*
 * The sections of a text: the lines of each one joined (a blank line gives a new paragraph), comments left out.
 * out[s] stays NULL for a section the text does not have.
 */
static void parse(const char *text, char *out[PERSONA_SECTIONS])
{
    int section = -1;
    bool paragraph = false;
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        const char *next = end ? end + 1 : NULL;
        while (len && (unsigned char)line[len - 1] <= ' ') {
            len--;
        }
        while (len && (unsigned char)*line <= ' ') {
            line++;
            len--;
        }
        if (len >= 2 && line[0] == '[' && line[len - 1] == ']') {
            section = section_of(line + 1, len - 2);
            if (section >= 0 && !out[section]) {
                out[section] = heap_caps_calloc(1, PERSONA_FILE_MAX, MALLOC_CAP_SPIRAM);
            }
            paragraph = false;
        } else if (section >= 0 && out[section] && len && line[0] != '#') {
            char *s = out[section];
            size_t used = strlen(s);
            if (used && used + len + 2 < PERSONA_FILE_MAX) {
                strcat(s, paragraph ? "\n" : " ");
            }
            if (strlen(s) + len + 1 < PERSONA_FILE_MAX) {
                strncat(s, line, len);
            }
            paragraph = false;
        } else if (section >= 0 && !len) {
            paragraph = out[section] && out[section][0];
        }
        line = next;
    }
    // Kept at their size
    for (int s = 0; s < PERSONA_SECTIONS; s++) {
        char *small = out[s] ? heap_caps_realloc(out[s], strlen(out[s]) + 1, MALLOC_CAP_SPIRAM) : NULL;
        out[s] = small ? small : out[s];
    }
}

/* The firmware's file, the old PERSONA.TXT (if any) as its [personality] */
static bool install(const char *code, const char *path, const char *old_persona)
{
    const char *def = default_text(code);
    if (!def) {
        return false;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Could not install %s", path);
        return false;
    }
    bool moved = false;
    // The personality section: its header, its comment lines, then the old text, before the next section
    const char *header = NULL;
    for (const char *p = def; (p = strchr(p, '[')) != NULL; p++) {
        const char *close = strchr(p, ']');
        if ((p == def || p[-1] == '\n') && close && section_of(p + 1, (size_t)(close - p - 1)) == PERSONA_PERSONALITY) {
            header = p;
            break;
        }
    }
    const char *next = header ? strstr(header, "\n[") : NULL;
    if (old_persona && old_persona[0] && next) {
        // "...comment\n" + the old text + "\n" + "\n[next section]..."
        fwrite(def, 1, (size_t)(next + 1 - def), f);
        fprintf(f, "%s\n", old_persona);
        fputs(next, f);
        moved = true;
    } else {
        fputs(def, f);
    }
    fclose(f);
    ESP_LOGI(TAG, "Installed %s%s", path, moved ? " (with the personality of PERSONA.TXT)" : "");
    return moved;
}

bool persona_init(const char *lang_code, const char *old_persona)
{
    // The old PERSONA.TXT goes to the file of the language in use (French for a language without its own file)
    const char *migrate = default_text(lang_code) ? lang_code : "fr";
    bool moved = false;
    for (size_t i = 0; i < sizeof(s_defaults) / sizeof(s_defaults[0]); i++) {
        char path[48];
        snprintf(path, sizeof(path), PERSONA_FILE_FORMAT, s_defaults[i].code);
        char *text = read_file(path);
        if (is_blank(text)) {
            bool here = strcasecmp(s_defaults[i].code, migrate) == 0;
            moved |= install(s_defaults[i].code, path, here ? old_persona : NULL);
        } else if (strcasecmp(s_defaults[i].code, migrate) == 0 && old_persona && old_persona[0]) {
            moved = true;   // a file of this firmware already there: the old text was moved before
        }
        free(text);
    }
    return moved;
}

void persona_load(const char *lang_code)
{
    char path[48];
    snprintf(path, sizeof(path), PERSONA_FILE_FORMAT, lang_code);
    char *text = read_file(path);
    const char *code = lang_code;
    if (!text && !default_text(lang_code)) {
        code = PERSONA_FALLBACK_LANG;
        snprintf(path, sizeof(path), PERSONA_FILE_FORMAT, code);
        text = read_file(path);
    }
    for (int s = 0; s < PERSONA_SECTIONS; s++) {
        free(s_sections[s]);
        s_sections[s] = NULL;
    }
    parse(text ? text : "", s_sections);
    free(text);
    // A section the file does not have: the firmware's
    const char *def = default_text(code);
    char *defaults[PERSONA_SECTIONS] = { NULL };
    if (def) {
        parse(def, defaults);
    }
    for (int s = 0; s < PERSONA_SECTIONS; s++) {
        if (!s_sections[s]) {
            s_sections[s] = defaults[s];
        } else {
            free(defaults[s]);
        }
    }
    ESP_LOGI(TAG, "%s: identity %u, personality %u, instructions %u, voice %u characters", path,
             (unsigned)strlen(persona_get(PERSONA_IDENTITY)), (unsigned)strlen(persona_get(PERSONA_PERSONALITY)),
             (unsigned)strlen(persona_get(PERSONA_INSTRUCTIONS)), (unsigned)strlen(persona_get(PERSONA_VOICE)));
}

const char *persona_get(persona_section_t section)
{
    return section < PERSONA_SECTIONS && s_sections[section] ? s_sections[section] : "";
}
