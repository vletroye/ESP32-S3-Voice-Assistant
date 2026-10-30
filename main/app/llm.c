/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <ctype.h>
#include <strings.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "cJSON.h"
#include "llm.h"
#include "ai_provider_anthropic.h"
#include "ai_provider_openai.h"
#include "ai_provider_google.h"
#include "ai_provider_openrouter.h"
#include "ai_provider_xai.h"
#include "ai_provider_mistral.h"
#include "ai_provider_perplexity.h"
#include "tools.h"

static const char *TAG = "llm";

/* ---------- the models offered: AGENTS.INI ---------- */

#define AGENTS_FILE         "/spiffs/agents.ini"
#define AGENTS_FILE_MAX     8192

/* The firmware's version (main/agents/agents.ini, embedded NUL-terminated) */
extern const char s_agents_default_start[] asm("_binary_agents_ini_start");
extern const char s_agents_default_end[] asm("_binary_agents_ini_end");

/* PSRAM (internal RAM is scarce): LLM_FILE_MODELS_MAX + LLM_OPENROUTER_FREE_MAX entries, by llm_load_models() */
llm_model_t *llm_models = NULL;
size_t llm_model_count = 0;
llm_stats_t llm_last_stats;
/* Models of AGENTS.INI, then the free OpenRouter ones */
static size_t s_file_count = 0;
/* Free OpenRouter models never offered: [openrouter_free_exclude] of AGENTS.INI */
#define FREE_EXCLUDED_MAX   32
#define FREE_EXCLUDE_SECTION    "openrouter_free_exclude"
/* [speech]: the transcription and speech models with their prices; [per_call]: prices per call (web search) */
#define SPEECH_SECTION          "speech"
#define PER_CALL_SECTION        "per_call"
#define SPEECH_MODELS_MAX       8
#define CALL_PRICES_MAX         8
static struct {
    char *id;
    double price_in;
    double price_out;
} s_speech[SPEECH_MODELS_MAX];
static int s_speech_count = 0;
static struct {
    char *id;
    double price;
} s_calls[CALL_PRICES_MAX];
static int s_call_count = 0;
static char *s_free_excluded[FREE_EXCLUDED_MAX];
static int s_free_excluded_count = 0;

static const struct {
    const char *section;
    llm_provider_t provider;
} s_sections[] = {
    { "anthropic", LLM_PROVIDER_CLAUDE },
    { "openai", LLM_PROVIDER_OPENAI },
    { "google", LLM_PROVIDER_GEMINI },
    { "openrouter", LLM_PROVIDER_OPENROUTER },
    { "xai", LLM_PROVIDER_XAI },
    { "mistral", LLM_PROVIDER_MISTRAL },
    { "perplexity", LLM_PROVIDER_PERPLEXITY },
};

/* Without the blanks around it (in place) */
static char *trim(char *s)
{
    while (*s && (unsigned char)*s <= ' ') {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && (unsigned char)s[n - 1] <= ' ') {
        s[--n] = '\0';
    }
    return s;
}

/* Values of the speed column (model test) */
static bool is_speed(const char *value)
{
    return strcmp(value, "fast") == 0 || strcmp(value, "average") == 0 || strcmp(value, "slow") == 0;
}

/* A price per million tokens (or per call), in the currency of CONFIG.INI: "1.72" or "1,72" */
static bool parse_price(const char *text, double *value)
{
    char buf[24];
    strlcpy(buf, text ? text : "", sizeof(buf));
    for (char *c = buf; *c; c++) {
        *c = (*c == ',') ? '.' : *c;
    }
    char *end = NULL;
    double v = strtod(buf, &end);
    if (!buf[0] || end == buf || *trim(end)) {
        return false;
    }
    *value = v;
    return true;
}

/* The fields after "id =": label | effort | spoken | price in | price out | speed | thinking; the last two are
 * optional and told apart by their values (pointers into the split line) */
typedef struct {
    const char *label;
    const char *effort;
    const char *spoken;
    const char *speed;
    const char *thinking;
    double price_in;
    double price_out;
    bool priced;
} fields_t;

static void split_fields(char *rest, fields_t *f)
{
    memset(f, 0, sizeof(*f));
    char *parts[8];
    int n = 0;
    for (char *r = rest; r && n < 8;) {
        char *bar = strchr(r, '|');
        if (bar) {
            *bar = '\0';
        }
        parts[n++] = trim(r);
        r = bar ? bar + 1 : NULL;
    }
    f->label = n > 0 ? parts[0] : "";
    f->effort = n > 1 ? parts[1] : "";
    f->spoken = n > 2 ? parts[2] : "";
    double prices[2];
    int priced = 0;
    for (int i = 3; i < n; i++) {
        if (!parts[i][0]) {
            continue;
        }
        if (priced < 2 && parse_price(parts[i], &prices[priced])) {
            priced++;
        } else if (is_speed(parts[i])) {
            f->speed = parts[i];
        } else {
            f->thinking = parts[i];
        }
    }
    if (priced == 2) {
        f->price_in = prices[0];
        f->price_out = prices[1];
        f->priced = true;
    }
}

/* Section of a line ("[anthropic]"): its provider, or one of the special sections */
typedef enum { SECTION_NONE, SECTION_MODELS, SECTION_EXCLUDE, SECTION_SPEECH, SECTION_PER_CALL } section_t;

static section_t section_of(char *name, int *provider)
{
    *provider = -1;
    name = trim(name);
    for (size_t i = 0; i < sizeof(s_sections) / sizeof(s_sections[0]); i++) {
        if (strcasecmp(name, s_sections[i].section) == 0) {
            *provider = s_sections[i].provider;
            return SECTION_MODELS;
        }
    }
    return strcasecmp(name, FREE_EXCLUDE_SECTION) == 0 ? SECTION_EXCLUDE :
           strcasecmp(name, SPEECH_SECTION) == 0 ? SECTION_SPEECH :
           strcasecmp(name, PER_CALL_SECTION) == 0 ? SECTION_PER_CALL : SECTION_NONE;
}

/*
 * The models of AGENTS.INI into models (at most max); with lists, also the excluded free models, the speech
 * models and the per-call prices
 */
static size_t parse_models(const char *text, llm_model_t *models, size_t max, bool lists)
{
    char *copy = strdup(text);
    size_t count = 0;
    section_t section = SECTION_NONE;
    int provider = -1;
    if (lists) {
        for (int i = 0; i < s_free_excluded_count; i++) {
            free(s_free_excluded[i]);
        }
        s_free_excluded_count = 0;
        s_speech_count = 0;
        s_call_count = 0;
    }
    for (char *line = copy ? strtok(copy, "\n") : NULL; line && count < max; line = strtok(NULL, "\n")) {
        line = trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line[0] == '[') {
            char *close = strchr(line, ']');
            if (close && *trim(close + 1)) {
                continue;   // "[speech]: text": not a header
            }
            if (close) {
                *close = '\0';
            }
            section = section_of(line + 1, &provider);
            if (section == SECTION_NONE && lists) {
                ESP_LOGW(TAG, "AGENTS.INI: [%s] is not a known section: its lines are ignored", line + 1);
            }
            continue;
        }
        if (section == SECTION_EXCLUDE) {
            // "vendor/model:free" (anything after '=' or a blank is a comment)
            line[strcspn(line, " \t=")] = '\0';
            if (lists && line[0] && s_free_excluded_count < FREE_EXCLUDED_MAX) {
                s_free_excluded[s_free_excluded_count++] = strdup(line);
            }
            continue;
        }
        char *eq = strchr(line, '=');
        if (section == SECTION_NONE || !eq) {
            continue;
        }
        *eq = '\0';
        const char *id = trim(line);
        if (section == SECTION_PER_CALL) {
            // "claude:web_search = 0.0086"
            double price;
            if (lists && id[0] && s_call_count < CALL_PRICES_MAX && parse_price(trim(eq + 1), &price)) {
                s_calls[s_call_count].id = strdup(id);
                s_calls[s_call_count++].price = price;
            }
            continue;
        }
        fields_t f;
        split_fields(eq + 1, &f);
        if (section == SECTION_SPEECH) {
            if (lists && id[0] && s_speech_count < SPEECH_MODELS_MAX) {
                s_speech[s_speech_count].id = strdup(id);
                s_speech[s_speech_count].price_in = f.price_in;
                s_speech[s_speech_count++].price_out = f.price_out;
                if (!f.priced) {
                    ESP_LOGW(TAG, "AGENTS.INI: %s has no prices: its cost is not counted", id);
                }
            }
            continue;
        }
        if (!id[0] || !f.label[0]) {
            ESP_LOGW(TAG, "AGENTS.INI: line without a model id or a name, ignored");
            continue;
        }
        llm_model_t *m = &models[count++];
        m->id = strdup(id);
        m->label = strdup(f.label);
        m->effort = f.effort[0] ? strdup(f.effort) : "low";
        m->spoken = strdup(f.spoken[0] ? f.spoken : f.label);
        m->speed = f.speed ? strdup(f.speed) : NULL;
        m->thinking = f.thinking ? strdup(f.thinking) : NULL;
        m->price_in = f.price_in;
        m->price_out = f.price_out;
        m->priced = f.priced;
        m->provider = (llm_provider_t)provider;
        if (!f.priced && lists) {
            ESP_LOGW(TAG, "AGENTS.INI: %s has no prices: its cost is not counted", id);
        }
    }
    free(copy);
    return count;
}

static char *read_agents_file(void)
{
    FILE *f = fopen(AGENTS_FILE, "r");
    if (!f) {
        return NULL;
    }
    char *text = malloc(AGENTS_FILE_MAX);
    size_t n = text ? fread(text, 1, AGENTS_FILE_MAX - 1, f) : 0;
    fclose(f);
    if (text) {
        text[n] = '\0';
    }
    return text;
}

static bool write_agents_file(const char *text)
{
    FILE *f = fopen(AGENTS_FILE, "w");
    bool ok = f && fputs(text, f) >= 0;
    if (f) {
        ok &= (fclose(f) == 0);
    }
    return ok;
}

/* The prices of a model in the firmware's AGENTS.INI (to complete a file of an older firmware) */
static bool default_prices(const char *id, double *price_in, double *price_out)
{
    llm_model_t *defaults = heap_caps_calloc(LLM_FILE_MODELS_MAX, sizeof(llm_model_t), MALLOC_CAP_SPIRAM);
    size_t n = defaults ? parse_models(s_agents_default_start, defaults, LLM_FILE_MODELS_MAX, false) : 0;
    bool found = false;
    for (size_t i = 0; i < n; i++) {
        if (!found && defaults[i].priced && strcmp(defaults[i].id, id) == 0) {
            *price_in = defaults[i].price_in;
            *price_out = defaults[i].price_out;
            found = true;
        }
        free((char *)defaults[i].id);
        free((char *)defaults[i].label);
        free((char *)defaults[i].spoken);
        free((char *)defaults[i].speed);
        free((char *)defaults[i].thinking);
        if (defaults[i].effort && strcmp(defaults[i].effort, "low") != 0) {
            free((char *)defaults[i].effort);
        }
    }
    free(defaults);
    return found;
}

/* "1.72" without useless zeros, "0.0086" */
static void price_text(char *buf, size_t size, double price)
{
    snprintf(buf, size, "%.4f", price);
    char *dot = strchr(buf, '.');
    if (dot) {
        char *end = buf + strlen(buf) - 1;
        while (end > dot + 2 && *end == '0') {
            *end-- = '\0';
        }
    }
}

/*
 * AGENTS.INI written again with, for its model lines: the speed given (speeds[i] for llm_models[i], NULL:
 * unchanged), and the prices of the firmware's file when a line has none. Comments and other lines are kept.
 * Returns the number of lines changed.
 */
static int rewrite_agents(const char *const *speeds)
{
    char *text = read_agents_file();
    if (!text) {
        return 0;
    }
    size_t size = strlen(text) + LLM_FILE_MODELS_MAX * 48 + 64;
    char *out = calloc(1, size);
    char *line_buf = malloc(512);
    if (!out || !line_buf) {
        free(text);
        free(out);
        free(line_buf);
        return 0;
    }
    int changed = 0;
    section_t section = SECTION_NONE;
    int provider = -1;
    for (char *line = text; line && *line;) {
        char *end = strchr(line, '\n');
        if (end) {
            *end = '\0';
        }
        size_t len = strlen(line);
        bool cr = len && line[len - 1] == '\r';
        if (cr) {
            line[--len] = '\0';
        }
        // The section and the model of the line, as parse_models() reads them
        char *copy = strdup(line);
        char *t = copy ? trim(copy) : NULL;
        bool rebuilt = false;
        char *close = (t && t[0] == '[') ? strchr(t, ']') : NULL;
        if (close && *trim(close + 1)) {
            // "[speech]: text", a comment an older firmware wrote without its '#': made a comment again
            snprintf(line_buf, 512, "# %s", line);
            rebuilt = true;
            changed++;
        } else if (t && t[0] == '[') {
            if (close) {
                *close = '\0';
            }
            section = section_of(t + 1, &provider);
        } else if (t && section == SECTION_MODELS && t[0] != '#' && t[0] != ';' && strchr(t, '=')) {
            char *eq = strchr(t, '=');
            *eq = '\0';
            const char *id = trim(t);
            fields_t f;
            split_fields(eq + 1, &f);
            const char *speed = f.speed;
            for (size_t i = 0; speeds && i < s_file_count; i++) {
                if (speeds[i] && strcmp(id, llm_models[i].id) == 0) {
                    speed = speeds[i];
                }
            }
            double price_in = f.price_in, price_out = f.price_out;
            bool priced = f.priced || default_prices(id, &price_in, &price_out);
            if (priced != f.priced || speed != f.speed) {
                // "id = label | effort | spoken | in | out[ | speed][ | thinking]", the id part as written
                char in_text[16], out_text[16];
                price_text(in_text, sizeof(in_text), price_in);
                price_text(out_text, sizeof(out_text), price_out);
                const char *line_eq = strchr(line, '=');
                snprintf(line_buf, 512, "%.*s= %s | %s | %s", (int)(line_eq - line), line, f.label,
                         f.effort[0] ? f.effort : "low", f.spoken[0] ? f.spoken : f.label);
                if (priced) {
                    strlcat(line_buf, " | ", 512);
                    strlcat(line_buf, in_text, 512);
                    strlcat(line_buf, " | ", 512);
                    strlcat(line_buf, out_text, 512);
                }
                if (speed) {
                    strlcat(line_buf, " | ", 512);
                    strlcat(line_buf, speed, 512);
                }
                if (f.thinking) {
                    strlcat(line_buf, " | ", 512);
                    strlcat(line_buf, f.thinking, 512);
                }
                rebuilt = true;
                changed++;
            }
        }
        free(copy);
        strlcat(out, rebuilt ? line_buf : line, size);
        if (end) {
            strlcat(out, cr ? "\r\n" : "\n", size);
        }
        line = end ? end + 1 : NULL;
    }
    if (changed) {
        bool ok = write_agents_file(out);
        ESP_LOGI(TAG, "%s %d line(s) of %s", ok ? "Rewrote" : "Could not rewrite", changed, AGENTS_FILE);
    }
    free(text);
    free(out);
    free(line_buf);
    return changed;
}

/* The line "[section]" (alone on its line, blanks aside) of a text, NULL if none: not "# [section]: ..." */
static const char *section_line(const char *text, const char *header)
{
    size_t len = strlen(header);
    for (const char *p = strstr(text, header); p; p = strstr(p + 1, header)) {
        const char *after = p + len;
        while (*after == ' ' || *after == '\t' || *after == '\r') {
            after++;
        }
        if ((p == text || p[-1] == '\n') && (*after == '\n' || *after == '\0')) {
            return p;
        }
    }
    return NULL;
}

/*
 * A line of the firmware's [per_call] missing in the [per_call] of AGENTS.INI (written by an older firmware):
 * inserted under its header
 */
static bool add_missing_call(const char *text, const char *id)
{
    const char *header = section_line(text, "[" PER_CALL_SECTION "]");
    const char *def_header = section_line(s_agents_default_start, "[" PER_CALL_SECTION "]");
    const char *def_line = def_header ? strstr(def_header, id) : NULL;
    if (!header || !def_line || strstr(header, id)) {
        return false;
    }
    const char *def_end = strchr(def_line, '\n');
    const char *after = strchr(header, '\n');
    if (!def_end || !after) {
        return false;
    }
    FILE *f = fopen(AGENTS_FILE, "w");
    if (!f) {
        return false;
    }
    fwrite(text, 1, (size_t)(after + 1 - text), f);
    fwrite(def_line, 1, (size_t)(def_end + 1 - def_line), f);
    fputs(after + 1, f);
    fclose(f);
    ESP_LOGI(TAG, "%s added to %s", id, AGENTS_FILE);
    return true;
}

/* A section of the firmware's file missing in AGENTS.INI (written by an older firmware): appended to it */
static bool add_missing_section(const char *text, const char *section)
{
    char header[48];
    snprintf(header, sizeof(header), "[%s]", section);
    if (section_line(text, header)) {
        return false;
    }
    const char *from = section_line(s_agents_default_start, header);
    if (!from) {
        return false;
    }
    // The comment lines just above the section go with it
    const char *start = from;
    while (start > s_agents_default_start && start[-1] == '\n') {
        const char *prev = start - 1;
        while (prev > s_agents_default_start && prev[-1] != '\n') {
            prev--;
        }
        if (*prev != '#') {
            break;
        }
        start = prev;
    }
    // Up to the comment lines of the next section, or to the end
    const char *next = strstr(from + 1, "\n[");
    const char *next_comment = strstr(from + 1, "\n#");
    if (next_comment && (!next || next_comment < next)) {
        next = next_comment;
    }
    size_t len = next ? (size_t)(next + 1 - start) : strlen(start);
    FILE *f = fopen(AGENTS_FILE, "a");
    if (!f) {
        return false;
    }
    fputs("\n", f);
    fwrite(start, 1, len, f);
    fclose(f);
    ESP_LOGI(TAG, "%s added to %s", header, AGENTS_FILE);
    return true;
}

void llm_load_models(void)
{
    if (!llm_models) {
        llm_models = heap_caps_calloc(LLM_FILE_MODELS_MAX + LLM_OPENROUTER_FREE_MAX, sizeof(llm_model_t),
                                      MALLOC_CAP_SPIRAM);
        assert(llm_models);
    }
    char *text = read_agents_file();
    bool blank = true;
    for (const char *c = text; c && *c && blank; c++) {
        blank = (unsigned char)*c <= ' ';
    }
    if (blank) {
        // Missing (first start) or emptied on the configuration drive: the version given with the firmware
        size_t len = (size_t)(s_agents_default_end - s_agents_default_start) - 1;     // without the added NUL
        FILE *f = fopen(AGENTS_FILE, "w");
        bool ok = f && fwrite(s_agents_default_start, 1, len, f) == len;
        if (f) {
            ok &= (fclose(f) == 0);
        }
        ESP_LOGI(TAG, "%s %s (%u bytes)", ok ? "Installed" : "Could not install", AGENTS_FILE, (unsigned)len);
    } else {
        // A file of an older firmware: the sections added since, and the prices of the models it lacks. The
        // [per_call] line first: it rewrites the file from text, which the sections appended would not be in
        add_missing_call(text, "perplexity:web_search");
        add_missing_section(text, FREE_EXCLUDE_SECTION);
        add_missing_section(text, SPEECH_SECTION);
        add_missing_section(text, PER_CALL_SECTION);
        add_missing_section(text, "xai");          // Grok models, offered since xAI is supported
        add_missing_section(text, "mistral");      // and the providers added since
        add_missing_section(text, "perplexity");
        rewrite_agents(NULL);
    }
    free(text);
    text = read_agents_file();
    s_file_count = parse_models(text ? text : s_agents_default_start, llm_models, LLM_FILE_MODELS_MAX, true);
    if (s_file_count == 0) {
        ESP_LOGE(TAG, "No model in AGENTS.INI: the firmware's list is used");
        s_file_count = parse_models(s_agents_default_start, llm_models, LLM_FILE_MODELS_MAX, true);
    }
    free(text);
    llm_model_count = s_file_count;
    ESP_LOGI(TAG, "%u models from AGENTS.INI, %d free OpenRouter model(s) excluded, %d speech model(s), "
             "%d per-call price(s)", (unsigned)s_file_count, s_free_excluded_count, s_speech_count, s_call_count);
}

bool llm_price_of(const char *id, double *price_in, double *price_out)
{
    for (size_t i = 0; i < llm_model_count; i++) {
        if (llm_models[i].priced && strcmp(id, llm_models[i].id) == 0) {
            *price_in = llm_models[i].price_in;
            *price_out = llm_models[i].price_out;
            return true;
        }
    }
    for (int i = 0; i < s_speech_count; i++) {
        if (strcmp(id, s_speech[i].id) == 0) {
            *price_in = s_speech[i].price_in;
            *price_out = s_speech[i].price_out;
            return true;
        }
    }
    return false;
}

bool llm_call_price_of(const char *id, double *price)
{
    for (int i = 0; i < s_call_count; i++) {
        if (strcmp(id, s_calls[i].id) == 0) {
            *price = s_calls[i].price;
            return true;
        }
    }
    return false;
}

bool llm_free_excluded(const char *id)
{
    for (int i = 0; i < s_free_excluded_count; i++) {
        const char *x = s_free_excluded[i];
        size_t len = strlen(x);
        if (strcmp(id, x) == 0 || (len && x[len - 1] == '*' && strncmp(id, x, len - 1) == 0)) {
            return true;
        }
    }
    return false;
}

void llm_free_exclude_add(const char *id, const char *comment)
{
    if (llm_free_excluded(id) || s_free_excluded_count >= FREE_EXCLUDED_MAX) {
        return;
    }
    s_free_excluded[s_free_excluded_count++] = strdup(id);
    char *text = read_agents_file();
    size_t size = (text ? strlen(text) : 0) + strlen(id) + (comment ? strlen(comment) : 0) + 96;
    char *out = calloc(1, size);
    if (!out) {
        free(text);
        return;
    }
    char line[160];
    snprintf(line, sizeof(line), "%s%s%s\n", id, comment ? "   # " : "", comment ? comment : "");
    const char *header = text ? strstr(text, "[" FREE_EXCLUDE_SECTION "]") : NULL;
    const char *after = header ? strchr(header, '\n') : NULL;
    if (after) {
        // Right under the section's header
        strncat(out, text, (size_t)(after + 1 - text));
        strlcat(out, line, size);
        strlcat(out, after + 1, size);
    } else {
        strlcat(out, text ? text : "", size);
        size_t len = strlen(out);
        strlcat(out, (len && out[len - 1] != '\n') ? "\n" : "", size);
        strlcat(out, header ? "\n" : "\n[" FREE_EXCLUDE_SECTION "]\n", size);
        strlcat(out, line, size);
    }
    FILE *f = fopen(AGENTS_FILE, "w");
    bool ok = f && fputs(out, f) >= 0;
    if (f) {
        ok &= (fclose(f) == 0);
    }
    ESP_LOGI(TAG, "%s %s excluded in %s", ok ? "" : "Could not write:", id, AGENTS_FILE);
    free(text);
    free(out);
}

size_t llm_agents_count(void)
{
    return s_file_count;
}

void llm_save_speeds(const char *const *speeds)
{
    rewrite_agents(speeds);
}

void llm_set_openrouter_free_models(const llm_model_t *models, size_t count)
{
    // Copied after the models of AGENTS.INI; the strings of a former list are not freed: a task may still be
    // reading them (a few hundred bytes a day)
    size_t n = 0;
    for (size_t i = 0; i < count && n < LLM_OPENROUTER_FREE_MAX; i++) {
        llm_model_t *m = &llm_models[s_file_count + n++];
        m->id = strdup(models[i].id);
        m->label = strdup(models[i].label);
        m->effort = models[i].effort;
        m->spoken = strdup(models[i].spoken);
        m->thinking = NULL;
        m->speed = NULL;
        m->price_in = 0;        // free
        m->price_out = 0;
        m->priced = true;
        m->provider = LLM_PROVIDER_OPENROUTER;
    }
    llm_model_count = s_file_count + n;
}

static const llm_model_t *model_of(const char *id)
{
    for (size_t i = 0; i < llm_model_count; i++) {
        if (strcmp(id, llm_models[i].id) == 0) {
            return &llm_models[i];
        }
    }
    return NULL;
}

/* Conversation history kept across sleep and restarts (SPIFFS "storage" partition) */
#define LLM_HISTORY_FILE        "/spiffs/history.json"
#define LLM_HISTORY_TMP_FILE    "/spiffs/history.tmp"

static llm_provider_t s_provider = LLM_PROVIDER_CLAUDE;
/* Keys and instructions of llm_init(): the model test configures other models with them */
static char *s_claude_key = NULL;
static char *s_openai_key = NULL;
static char *s_openai_url = NULL;
static char *s_gemini_key = NULL;
static char *s_openrouter_key = NULL;
static char *s_xai_key = NULL;
static char *s_mistral_key = NULL;
static char *s_perplexity_key = NULL;
static char *s_system = NULL;
static bool s_ready = false;
static char s_model[64];
/* Held while the history is used (stream, save) */
static SemaphoreHandle_t s_lock = NULL;
/* History changed since the last save */
static bool s_dirty = false;

llm_provider_t llm_provider_of(const char *model)
{
    const llm_model_t *listed = model_of(model);
    if (listed) {
        return listed->provider;
    }
    if (strchr(model, '/')) {
        return LLM_PROVIDER_OPENROUTER;     // "vendor/model"
    }
    if (strncmp(model, "gpt-", 4) == 0 || (model[0] == 'o' && isdigit((unsigned char)model[1]))) {
        return LLM_PROVIDER_OPENAI;
    }
    if (strncmp(model, "gemini-", 7) == 0) {
        return LLM_PROVIDER_GEMINI;
    }
    if (strncmp(model, "grok-", 5) == 0) {
        return LLM_PROVIDER_XAI;
    }
    if (strncmp(model, "mistral-", 8) == 0 || strncmp(model, "magistral-", 10) == 0 ||
            strncmp(model, "ministral-", 10) == 0) {
        return LLM_PROVIDER_MISTRAL;
    }
    if (strncmp(model, "sonar", 5) == 0) {
        return LLM_PROVIDER_PERPLEXITY;
    }
    return LLM_PROVIDER_CLAUDE;
}

const char *llm_provider_name(llm_provider_t provider)
{
    return provider == LLM_PROVIDER_OPENAI ? "GPT" : provider == LLM_PROVIDER_GEMINI ? "Gemini" :
           provider == LLM_PROVIDER_OPENROUTER ? "OpenRouter" : provider == LLM_PROVIDER_XAI ? "Grok" :
           provider == LLM_PROVIDER_MISTRAL ? "Mistral" : provider == LLM_PROVIDER_PERPLEXITY ? "Perplexity" : "Claude";
}

/* Effort of a listed model; "low" for a model only known from CONFIG.INI (accepted by every recent model) */
char llm_effort_refused[16];
char llm_effort_values[64];

static const char *effort_of(const char *model)
{
    for (size_t i = 0; i < llm_model_count; i++) {
        if (strcmp(model, llm_models[i].id) == 0) {
            return llm_models[i].effort;
        }
    }
    return "low";
}

static const cJSON *history_get(void)
{
    return s_provider == LLM_PROVIDER_OPENAI ? openai_chat_history() :
           s_provider == LLM_PROVIDER_GEMINI ? gemini_chat_history() :
           s_provider == LLM_PROVIDER_OPENROUTER ? openrouter_chat_history() :
           s_provider == LLM_PROVIDER_XAI ? xai_chat_history() :
           s_provider == LLM_PROVIDER_MISTRAL ? mistral_chat_history() :
           s_provider == LLM_PROVIDER_PERPLEXITY ? perplexity_chat_history() : claude_client_history();
}

/* Give a conversation (taken over) to the client of the current provider */
static void history_set(cJSON *messages)
{
    if (s_provider == LLM_PROVIDER_OPENAI) {
        openai_chat_set_history(messages);
    } else if (s_provider == LLM_PROVIDER_GEMINI) {
        gemini_chat_set_history(messages);
    } else if (s_provider == LLM_PROVIDER_OPENROUTER) {
        openrouter_chat_set_history(messages);
    } else if (s_provider == LLM_PROVIDER_XAI) {
        xai_chat_set_history(messages);
    } else if (s_provider == LLM_PROVIDER_MISTRAL) {
        mistral_chat_set_history(messages);
    } else if (s_provider == LLM_PROVIDER_PERPLEXITY) {
        perplexity_chat_set_history(messages);
    } else {
        claude_client_set_history(messages);
    }
}

/* Reload the conversation saved for the same model (another model's history has another format) */
static void history_restore(void)
{
    FILE *f = fopen(LLM_HISTORY_FILE, "r");
    if (!f) {
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = size > 0 ? malloc(size + 1) : NULL;
    size_t n = text ? fread(text, 1, size, f) : 0;
    fclose(f);
    if (!text) {
        return;
    }
    text[n] = '\0';
    cJSON *saved = cJSON_Parse(text);
    free(text);

    const cJSON *model = cJSON_GetObjectItem(saved, "model");
    cJSON *messages = cJSON_DetachItemFromObject(saved, "messages");
    if (cJSON_IsString(model) && strcmp(model->valuestring, s_model) == 0 && cJSON_IsArray(messages)) {
        int count = cJSON_GetArraySize(messages);
        history_set(messages);
        ESP_LOGI(TAG, "Conversation restored: %d messages", count);
    } else {
        ESP_LOGI(TAG, "Saved conversation ignored (other model or unreadable)");
        cJSON_Delete(messages);
    }
    cJSON_Delete(saved);
}

void llm_history_save(void)
{
    if (!s_ready || !s_dirty || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    cJSON *saved = cJSON_CreateObject();
    cJSON_AddStringToObject(saved, "model", s_model);
    cJSON_AddItemReferenceToObject(saved, "messages", (cJSON *)history_get());
    char *text = cJSON_PrintUnformatted(saved);
    cJSON_Delete(saved);
    s_dirty = false;
    xSemaphoreGive(s_lock);
    if (!text) {
        return;
    }

    // Written to a temporary file first: a power cut during the write keeps the previous conversation
    FILE *f = fopen(LLM_HISTORY_TMP_FILE, "w");
    size_t len = strlen(text);
    bool ok = f && fwrite(text, 1, len, f) == len;
    if (f) {
        ok &= (fclose(f) == 0);
    }
    free(text);
    if (ok) {
        remove(LLM_HISTORY_FILE);
        ok = (rename(LLM_HISTORY_TMP_FILE, LLM_HISTORY_FILE) == 0);
    }
    if (ok) {
        ESP_LOGI(TAG, "Conversation saved (%u bytes)", (unsigned)len);
    } else {
        ESP_LOGE(TAG, "Failed to save the conversation");
    }
}

static void keep(char **dst, const char *src)
{
    free(*dst);
    *dst = strdup(src ? src : "");
}

/* The client of the model's provider (s_provider) configured for it, with the keys and instructions kept */
static esp_err_t client_init(const char *model, bool tools)
{
    esp_err_t ret;
    if (s_provider == LLM_PROVIDER_OPENAI) {
        ret = openai_chat_init(s_openai_url, s_openai_key, model, s_system, effort_of(model));
    } else if (s_provider == LLM_PROVIDER_GEMINI) {
        if (s_gemini_key[0] == '\0') {
            ESP_LOGW(TAG, "No Gemini_key configured for %s", model);
            ret = ESP_ERR_INVALID_ARG;
        } else {
            ret = gemini_chat_init(s_gemini_key, model, s_system, effort_of(model));
        }
    } else if (s_provider == LLM_PROVIDER_OPENROUTER) {
        if (s_openrouter_key[0] == '\0') {
            ESP_LOGW(TAG, "No OpenRouter_key configured for %s", model);
            ret = ESP_ERR_INVALID_ARG;
        } else {
            ret = openrouter_chat_init(s_openrouter_key, model, s_system, effort_of(model));
        }
    } else if (s_provider == LLM_PROVIDER_XAI) {
        if (s_xai_key[0] == '\0') {
            ESP_LOGW(TAG, "No XAI_key configured for %s", model);
            ret = ESP_ERR_INVALID_ARG;
        } else {
            ret = xai_chat_init(s_xai_key, model, s_system, effort_of(model));
        }
    } else if (s_provider == LLM_PROVIDER_MISTRAL) {
        if (s_mistral_key[0] == '\0') {
            ESP_LOGW(TAG, "No Mistral_key configured for %s", model);
            ret = ESP_ERR_INVALID_ARG;
        } else {
            ret = mistral_chat_init(s_mistral_key, model, s_system, effort_of(model));
        }
    } else if (s_provider == LLM_PROVIDER_PERPLEXITY) {
        if (s_perplexity_key[0] == '\0') {
            ESP_LOGW(TAG, "No Perplexity_key configured for %s", model);
            ret = ESP_ERR_INVALID_ARG;
        } else {
            ret = perplexity_chat_init(s_perplexity_key, model, s_system, effort_of(model));
        }
    } else if (s_claude_key[0] == '\0') {
        ESP_LOGW(TAG, "No Claude_key configured for %s", model);
        ret = ESP_ERR_INVALID_ARG;
    } else {
        const llm_model_t *listed = model_of(model);
        ret = claude_client_init(s_claude_key, model, s_system, effort_of(model), listed ? listed->thinking : NULL);
        // Tools (weather, volume...): offered to Claude only for now; none for the model test
        claude_client_set_tools(tools ? tools_definitions() : NULL, tools ? tools_run : NULL);
    }
    return ret;
}

esp_err_t llm_init(const char *model, const char *claude_key, const char *openai_key, const char *openai_base_url,
                   const char *gemini_key, const char *openrouter_key, const char *xai_key,
                   const char *mistral_key, const char *perplexity_key, const char *system_prompt)
{
    // Called again when the model is changed at run time: wait for an answer being generated
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    keep(&s_claude_key, claude_key);
    keep(&s_openai_key, openai_key);
    keep(&s_openai_url, openai_base_url);
    keep(&s_gemini_key, gemini_key);
    keep(&s_openrouter_key, openrouter_key);
    keep(&s_xai_key, xai_key);
    keep(&s_mistral_key, mistral_key);
    keep(&s_perplexity_key, perplexity_key);
    keep(&s_system, system_prompt);
    s_provider = llm_provider_of(model);
    esp_err_t ret = client_init(model, true);
    s_ready = (ret == ESP_OK);
    strlcpy(s_model, model, sizeof(s_model));
    s_dirty = false;
    if (s_lock) {
        xSemaphoreGive(s_lock);
    } else {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_ready) {
        history_restore();      // the conversation saved for this model, if any
    }
    return ret;
}

void llm_set_system_prompt(const char *system_prompt)
{
    if (!s_ready) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    keep(&s_system, system_prompt);
    if (s_provider == LLM_PROVIDER_OPENAI) {
        openai_chat_set_instructions(system_prompt);
    } else if (s_provider == LLM_PROVIDER_GEMINI) {
        gemini_chat_set_instructions(system_prompt);
    } else if (s_provider == LLM_PROVIDER_OPENROUTER) {
        openrouter_chat_set_instructions(system_prompt);
    } else if (s_provider == LLM_PROVIDER_XAI) {
        xai_chat_set_instructions(system_prompt);
    } else if (s_provider == LLM_PROVIDER_MISTRAL) {
        mistral_chat_set_instructions(system_prompt);
    } else if (s_provider == LLM_PROVIDER_PERPLEXITY) {
        perplexity_chat_set_instructions(system_prompt);
    } else {
        claude_client_set_system(system_prompt);
    }
    xSemaphoreGive(s_lock);
}

void llm_set_tools(const cJSON *definitions)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    claude_client_set_tools(definitions, tools_run);
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

void llm_set_location(const char *country, const char *city)
{
    claude_client_set_location(country, city);
}

int llm_web_searches(void)
{
    return (s_ready && s_provider == LLM_PROVIDER_CLAUDE) ? claude_client_web_searches() : 0;
}

llm_provider_t llm_provider(void)
{
    return s_provider;
}

/* The question to the client of the current provider (s_lock held) */
static llm_error_t client_stream(const char *text, llm_text_cb_t on_text, void *ctx, char **full_text)
{
    return s_provider == LLM_PROVIDER_OPENAI ? openai_chat_stream(text, on_text, ctx, full_text)
           : s_provider == LLM_PROVIDER_GEMINI ? gemini_chat_stream(text, on_text, ctx, full_text)
           : s_provider == LLM_PROVIDER_OPENROUTER ? openrouter_chat_stream(text, on_text, ctx, full_text)
           : s_provider == LLM_PROVIDER_XAI ? xai_chat_stream(text, on_text, ctx, full_text)
           : s_provider == LLM_PROVIDER_MISTRAL ? mistral_chat_stream(text, on_text, ctx, full_text)
           : s_provider == LLM_PROVIDER_PERPLEXITY ? perplexity_chat_stream(text, on_text, ctx, full_text)
           : claude_client_stream(text, on_text, ctx, full_text);
}

llm_error_t llm_probe(const llm_model_t *model, const char *question, llm_stats_t *stats)
{
    if (!s_lock || !s_system) {
        *stats = (llm_stats_t){ -1, 0, 0, -1 };
        return LLM_ERR_NO_KEY;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // The conversation of the configured model, given back afterwards (its client may be the one tested)
    cJSON *saved = s_ready ? cJSON_Duplicate(history_get(), true) : NULL;
    llm_provider_t provider = s_provider;
    s_provider = llm_provider_of(model->id);
    llm_last_stats = (llm_stats_t){ -1, 0, 0, -1 };
    llm_error_t err = LLM_ERR_NO_KEY;
    if (client_init(model->id, false) == ESP_OK) {
        char *answer = NULL;
        err = client_stream(question, NULL, NULL, &answer);
        ESP_LOGI(TAG, "Test of %s: %.160s", model->id, answer ? answer : "-");
        free(answer);
    }
    *stats = llm_last_stats;
    s_provider = provider;
    if (s_ready) {
        client_init(s_model, true);
        history_set(saved);
    } else {
        cJSON_Delete(saved);
    }
    xSemaphoreGive(s_lock);
    return err;
}

static volatile bool s_cancelled = false;

void llm_cancel(void)
{
    s_cancelled = true;
}

void llm_cancel_clear(void)
{
    s_cancelled = false;
}

bool llm_cancelled(void)
{
    return s_cancelled;
}

llm_error_t llm_stream(const char *text, llm_text_cb_t on_text, void *ctx, char **full_text)
{
    if (full_text) {
        *full_text = NULL;
    }
    if (!s_ready) {
        return LLM_ERR_NO_KEY;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    llm_error_t err = client_stream(text, on_text, ctx, full_text);
    if (err == LLM_OK) {
        s_dirty = true;
    }
    xSemaphoreGive(s_lock);
    return err;
}

void llm_prewarm(void)
{
    if (!s_ready) {
        return;
    }
    if (s_provider == LLM_PROVIDER_OPENAI) {
        openai_chat_prewarm();
    } else if (s_provider == LLM_PROVIDER_GEMINI) {
        gemini_chat_prewarm();
    } else if (s_provider == LLM_PROVIDER_OPENROUTER) {
        openrouter_chat_prewarm();
    } else if (s_provider == LLM_PROVIDER_XAI) {
        xai_chat_prewarm();
    } else if (s_provider == LLM_PROVIDER_MISTRAL) {
        mistral_chat_prewarm();
    } else if (s_provider == LLM_PROVIDER_PERPLEXITY) {
        perplexity_chat_prewarm();
    } else {
        claude_client_prewarm();
    }
}

void llm_reset(void)
{
    if (!s_ready) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_provider == LLM_PROVIDER_OPENAI) {
        openai_chat_reset();
    } else if (s_provider == LLM_PROVIDER_GEMINI) {
        gemini_chat_reset();
    } else if (s_provider == LLM_PROVIDER_OPENROUTER) {
        openrouter_chat_reset();
    } else if (s_provider == LLM_PROVIDER_XAI) {
        xai_chat_reset();
    } else if (s_provider == LLM_PROVIDER_MISTRAL) {
        mistral_chat_reset();
    } else if (s_provider == LLM_PROVIDER_PERPLEXITY) {
        perplexity_chat_reset();
    } else {
        claude_client_reset();
    }
    s_dirty = false;
    xSemaphoreGive(s_lock);
    remove(LLM_HISTORY_FILE);
    ESP_LOGI(TAG, "Conversation cleared");
}
