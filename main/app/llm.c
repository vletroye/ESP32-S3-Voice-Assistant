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
#include "claude_client.h"
#include "openai_chat.h"
#include "gemini_chat.h"
#include "openrouter_chat.h"
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

/* "id = label | effort | spoken | speed | thinking" lines, by "[provider]" section, into llm_models; speed and
 * thinking are optional and told apart by their values */
static size_t parse_models(const char *text)
{
    char *copy = strdup(text);
    size_t count = 0;
    int provider = -1;      // section not known yet: its lines are ignored
    bool exclude = false;   // in [openrouter_free_exclude]: one model id per line
    for (int i = 0; i < s_free_excluded_count; i++) {
        free(s_free_excluded[i]);
    }
    s_free_excluded_count = 0;
    for (char *line = copy ? strtok(copy, "\n") : NULL; line && count < LLM_FILE_MODELS_MAX;
            line = strtok(NULL, "\n")) {
        line = trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line[0] == '[') {
            char *close = strchr(line, ']');
            if (close) {
                *close = '\0';
            }
            provider = -1;
            exclude = (strcasecmp(trim(line + 1), FREE_EXCLUDE_SECTION) == 0);
            for (size_t i = 0; i < sizeof(s_sections) / sizeof(s_sections[0]); i++) {
                if (strcasecmp(trim(line + 1), s_sections[i].section) == 0) {
                    provider = s_sections[i].provider;
                }
            }
            if (provider < 0 && !exclude) {
                ESP_LOGW(TAG, "AGENTS.INI: [%s] is not a known provider: its models are ignored", line + 1);
            }
            continue;
        }
        if (exclude) {
            // "vendor/model:free" (anything after '=' or a blank is a comment)
            line[strcspn(line, " \t=")] = '\0';
            if (line[0] && s_free_excluded_count < FREE_EXCLUDED_MAX) {
                s_free_excluded[s_free_excluded_count++] = strdup(line);
            }
            continue;
        }
        char *eq = strchr(line, '=');
        if (provider < 0 || !eq) {
            continue;
        }
        *eq = '\0';
        char *fields[5] = { NULL };
        char *rest = eq + 1;
        for (int f = 0; f < 5 && rest; f++) {
            char *bar = strchr(rest, '|');
            if (bar) {
                *bar = '\0';
            }
            fields[f] = trim(rest);
            rest = bar ? bar + 1 : NULL;
        }
        const char *id = trim(line);
        if (!id[0] || !fields[0] || !fields[0][0]) {
            ESP_LOGW(TAG, "AGENTS.INI: line without a model id or a name, ignored");
            continue;
        }
        llm_model_t *m = &llm_models[count++];
        m->id = strdup(id);
        m->label = strdup(fields[0]);
        m->effort = (fields[1] && fields[1][0]) ? strdup(fields[1]) : "low";
        m->spoken = strdup((fields[2] && fields[2][0]) ? fields[2] : fields[0]);
        m->thinking = NULL;
        m->speed = NULL;
        for (int f = 3; f < 5; f++) {
            if (fields[f] && fields[f][0]) {
                if (is_speed(fields[f])) {
                    m->speed = strdup(fields[f]);
                } else {
                    m->thinking = strdup(fields[f]);
                }
            }
        }
        m->provider = (llm_provider_t)provider;
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
        free(text);
        size_t len = (size_t)(s_agents_default_end - s_agents_default_start) - 1;     // without the added NUL
        FILE *f = fopen(AGENTS_FILE, "w");
        bool ok = f && fwrite(s_agents_default_start, 1, len, f) == len;
        if (f) {
            ok &= (fclose(f) == 0);
        }
        ESP_LOGI(TAG, "%s %s (%u bytes)", ok ? "Installed" : "Could not install", AGENTS_FILE, (unsigned)len);
        text = read_agents_file();
    } else if (!strstr(text, "[" FREE_EXCLUDE_SECTION "]")) {
        // A file of an older firmware: the section of the excluded free models is added, empty
        FILE *f = fopen(AGENTS_FILE, "a");
        if (f) {
            fputs("\n[" FREE_EXCLUDE_SECTION "]\n# Free OpenRouter models never offered, one id per line "
                  "(an id ending with * excludes those starting with it)\n", f);
            fclose(f);
            ESP_LOGI(TAG, "[%s] added to %s", FREE_EXCLUDE_SECTION, AGENTS_FILE);
        }
    }
    s_file_count = parse_models(text ? text : s_agents_default_start);
    if (s_file_count == 0) {
        ESP_LOGE(TAG, "No model in AGENTS.INI: the firmware's list is used");
        s_file_count = parse_models(s_agents_default_start);
    }
    free(text);
    llm_model_count = s_file_count;
    ESP_LOGI(TAG, "%u models from AGENTS.INI, %d free OpenRouter model(s) excluded", (unsigned)s_file_count,
             s_free_excluded_count);
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

/* A line of AGENTS.INI with its speed: "id = label | effort | spoken | speed[ | thinking]" (id part kept) */
static void line_with_speed(char *out, size_t size, const char *line, const char *speed)
{
    const char *eq = strchr(line, '=');
    char *copy = strdup(eq + 1);
    char *fields[8] = { NULL };
    int n = 0;
    for (char *rest = copy; rest && n < 8;) {
        char *bar = strchr(rest, '|');
        if (bar) {
            *bar = '\0';
        }
        fields[n++] = trim(rest);
        rest = bar ? bar + 1 : NULL;
    }
    snprintf(out, size, "%.*s= %s | %s | %s | %s", (int)(eq - line), line, n > 0 ? fields[0] : "",
             n > 1 && fields[1][0] ? fields[1] : "low", n > 2 && fields[2][0] ? fields[2] : fields[0], speed);
    for (int f = 3; f < n; f++) {
        if (fields[f][0] && !is_speed(fields[f])) {
            strlcat(out, " | ", size);
            strlcat(out, fields[f], size);
        }
    }
    free(copy);
}

void llm_save_speeds(const char *const *speeds)
{
    char *text = read_agents_file();
    if (!text) {
        return;
    }
    size_t size = strlen(text) + s_file_count * 16 + 64;
    char *out = calloc(1, size);
    char *line_buf = malloc(512);
    if (!out || !line_buf) {
        free(text);
        free(out);
        free(line_buf);
        return;
    }
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
        const char *speed = NULL;
        if (t && t[0] == '[') {
            char *close = strchr(t, ']');
            if (close) {
                *close = '\0';
            }
            provider = -1;
            for (size_t i = 0; i < sizeof(s_sections) / sizeof(s_sections[0]); i++) {
                if (strcasecmp(trim(t + 1), s_sections[i].section) == 0) {
                    provider = s_sections[i].provider;
                }
            }
        } else if (t && provider >= 0 && t[0] != '#' && t[0] != ';' && strchr(t, '=')) {
            *strchr(t, '=') = '\0';
            const char *id = trim(t);
            for (size_t i = 0; i < s_file_count; i++) {
                if (speeds[i] && strcmp(id, llm_models[i].id) == 0) {
                    speed = speeds[i];
                }
            }
        }
        free(copy);
        if (speed) {
            line_with_speed(line_buf, 512, line, speed);
            strlcat(out, line_buf, size);
        } else {
            strlcat(out, line, size);
        }
        if (end) {
            strlcat(out, cr ? "\r\n" : "\n", size);
        }
        line = end ? end + 1 : NULL;
    }
    FILE *f = fopen(AGENTS_FILE, "w");
    bool ok = f && fputs(out, f) >= 0;
    if (f) {
        ok &= (fclose(f) == 0);
    }
    ESP_LOGI(TAG, "%s the speeds to %s", ok ? "Written" : "Could not write", AGENTS_FILE);
    free(text);
    free(out);
    free(line_buf);
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
    return LLM_PROVIDER_CLAUDE;
}

const char *llm_provider_name(llm_provider_t provider)
{
    return provider == LLM_PROVIDER_OPENAI ? "GPT" : provider == LLM_PROVIDER_GEMINI ? "Gemini" :
           provider == LLM_PROVIDER_OPENROUTER ? "OpenRouter" : "Claude";
}

/* Effort of a listed model; "low" for a model only known from CONFIG.INI (accepted by every recent model) */
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
           s_provider == LLM_PROVIDER_OPENROUTER ? openrouter_chat_history() : claude_client_history();
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
                   const char *gemini_key, const char *openrouter_key, const char *system_prompt)
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
    } else {
        claude_client_reset();
    }
    s_dirty = false;
    xSemaphoreGive(s_lock);
    remove(LLM_HISTORY_FILE);
    ESP_LOGI(TAG, "Conversation cleared");
}
