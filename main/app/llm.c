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

llm_model_t llm_models[LLM_FILE_MODELS_MAX + LLM_OPENROUTER_FREE_MAX];
size_t llm_model_count = 0;
/* Models of AGENTS.INI, then the free OpenRouter ones */
static size_t s_file_count = 0;

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

/* "id = label | effort | spoken | thinking" lines, by "[provider]" section, into llm_models */
static size_t parse_models(const char *text)
{
    char *copy = strdup(text);
    size_t count = 0;
    int provider = -1;      // section not known yet: its lines are ignored
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
            for (size_t i = 0; i < sizeof(s_sections) / sizeof(s_sections[0]); i++) {
                if (strcasecmp(trim(line + 1), s_sections[i].section) == 0) {
                    provider = s_sections[i].provider;
                }
            }
            if (provider < 0) {
                ESP_LOGW(TAG, "AGENTS.INI: [%s] is not a known provider: its models are ignored", line + 1);
            }
            continue;
        }
        char *eq = strchr(line, '=');
        if (provider < 0 || !eq) {
            continue;
        }
        *eq = '\0';
        char *fields[4] = { NULL };
        char *rest = eq + 1;
        for (int f = 0; f < 4 && rest; f++) {
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
        m->thinking = (fields[3] && fields[3][0]) ? strdup(fields[3]) : NULL;
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
    }
    s_file_count = parse_models(text ? text : s_agents_default_start);
    if (s_file_count == 0) {
        ESP_LOGE(TAG, "No model in AGENTS.INI: the firmware's list is used");
        s_file_count = parse_models(s_agents_default_start);
    }
    free(text);
    llm_model_count = s_file_count;
    ESP_LOGI(TAG, "%u models from AGENTS.INI", (unsigned)s_file_count);
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
        if (s_provider == LLM_PROVIDER_OPENAI) {
            openai_chat_set_history(messages);
        } else if (s_provider == LLM_PROVIDER_GEMINI) {
            gemini_chat_set_history(messages);
        } else if (s_provider == LLM_PROVIDER_OPENROUTER) {
            openrouter_chat_set_history(messages);
        } else {
            claude_client_set_history(messages);
        }
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

esp_err_t llm_init(const char *model, const char *claude_key, const char *openai_key, const char *openai_base_url,
                   const char *gemini_key, const char *openrouter_key, const char *system_prompt)
{
    // Called again when the model is changed at run time: wait for an answer being generated
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_provider = llm_provider_of(model);
    esp_err_t ret;
    if (s_provider == LLM_PROVIDER_OPENAI) {
        ret = openai_chat_init(openai_base_url, openai_key, model, system_prompt, effort_of(model));
    } else if (s_provider == LLM_PROVIDER_GEMINI) {
        if (gemini_key == NULL || gemini_key[0] == '\0') {
            ESP_LOGW(TAG, "No Gemini_key configured for %s", model);
            ret = ESP_ERR_INVALID_ARG;
        } else {
            ret = gemini_chat_init(gemini_key, model, system_prompt, effort_of(model));
        }
    } else if (s_provider == LLM_PROVIDER_OPENROUTER) {
        if (openrouter_key == NULL || openrouter_key[0] == '\0') {
            ESP_LOGW(TAG, "No OpenRouter_key configured for %s", model);
            ret = ESP_ERR_INVALID_ARG;
        } else {
            ret = openrouter_chat_init(openrouter_key, model, system_prompt, effort_of(model));
        }
    } else if (claude_key == NULL || claude_key[0] == '\0') {
        ESP_LOGW(TAG, "No Claude_key configured for %s", model);
        ret = ESP_ERR_INVALID_ARG;
    } else {
        const llm_model_t *listed = model_of(model);
        ret = claude_client_init(claude_key, model, system_prompt, effort_of(model), listed ? listed->thinking : NULL);
        // Tools (weather, volume...): offered to Claude only for now
        claude_client_set_tools(tools_definitions(), tools_run);
    }
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

llm_error_t llm_stream(const char *text, llm_text_cb_t on_text, void *ctx, char **full_text)
{
    if (full_text) {
        *full_text = NULL;
    }
    if (!s_ready) {
        return LLM_ERR_NO_KEY;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    llm_error_t err = s_provider == LLM_PROVIDER_OPENAI ? openai_chat_stream(text, on_text, ctx, full_text)
                      : s_provider == LLM_PROVIDER_GEMINI ? gemini_chat_stream(text, on_text, ctx, full_text)
                      : s_provider == LLM_PROVIDER_OPENROUTER ? openrouter_chat_stream(text, on_text, ctx, full_text)
                      : claude_client_stream(text, on_text, ctx, full_text);
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
