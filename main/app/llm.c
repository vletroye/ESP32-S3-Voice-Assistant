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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "cJSON.h"
#include "llm.h"
#include "claude_client.h"
#include "openai_chat.h"
#include "tools.h"

static const char *TAG = "llm";

/*
 * Latest models of each provider. Effort is kept low for a fast spoken answer: GPT-6 Luna can skip
 * reasoning ("none"), GPT-6.1 Sol and GPT-6 Astra start at "low".
 */
const llm_model_t llm_models[] = {
    { "claude-haiku-5-5",  "Claude Haiku 5.5",  "low",  "Haiku" },
    { "claude-sonnet-5-5", "Claude Sonnet 5.5", "low",  "Sonnet" },
    { "claude-opus-5-5",   "Claude Opus 5.5",   "low",  "Opus" },
    { "gpt-6-luna",        "GPT-6 Luna",        "none", "Luna" },
    { "gpt-6.1-sol",       "GPT-6.1 Sol",       "low",  "Sol" },
    { "gpt-6-astra",       "GPT-6 Astra",       "low",  "Astra" },
};
const size_t llm_model_count = sizeof(llm_models) / sizeof(llm_models[0]);

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
    if (strncmp(model, "gpt-", 4) == 0 || (model[0] == 'o' && isdigit((unsigned char)model[1]))) {
        return LLM_PROVIDER_OPENAI;
    }
    return LLM_PROVIDER_CLAUDE;
}

const char *llm_provider_name(llm_provider_t provider)
{
    return provider == LLM_PROVIDER_OPENAI ? "GPT" : "Claude";
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
    return s_provider == LLM_PROVIDER_OPENAI ? openai_chat_history() : claude_client_history();
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
                   const char *system_prompt)
{
    // Called again when the model is changed at run time: wait for an answer being generated
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_provider = llm_provider_of(model);
    esp_err_t ret;
    if (s_provider == LLM_PROVIDER_OPENAI) {
        ret = openai_chat_init(openai_base_url, openai_key, model, system_prompt, effort_of(model));
    } else if (claude_key == NULL || claude_key[0] == '\0') {
        ESP_LOGW(TAG, "No Claude_key configured for %s", model);
        ret = ESP_ERR_INVALID_ARG;
    } else {
        ret = claude_client_init(claude_key, model, system_prompt, effort_of(model));
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
    } else {
        claude_client_set_system(system_prompt);
    }
    xSemaphoreGive(s_lock);
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
    } else {
        claude_client_reset();
    }
    s_dirty = false;
    xSemaphoreGive(s_lock);
    remove(LLM_HISTORY_FILE);
    ESP_LOGI(TAG, "Conversation cleared");
}
