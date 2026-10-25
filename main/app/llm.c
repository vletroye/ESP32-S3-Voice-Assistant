/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"
#include "llm.h"
#include "claude_client.h"
#include "openai_chat.h"

static const char *TAG = "llm";

/*
 * Latest models of each provider. Effort is kept low for a fast spoken answer: GPT-6 Luna can skip
 * reasoning ("none"), GPT-6.1 Sol and GPT-6 Astra start at "low".
 */
const llm_model_t llm_models[] = {
    { "claude-haiku-5-5",  "Claude Haiku 5.5",  "low" },
    { "claude-sonnet-5-5", "Claude Sonnet 5.5", "low" },
    { "claude-opus-5-5",   "Claude Opus 5.5",   "low" },
    { "gpt-6-luna",        "GPT-6 Luna",        "none" },
    { "gpt-6.1-sol",       "GPT-6.1 Sol",       "low" },
    { "gpt-6-astra",       "GPT-6 Astra",       "low" },
};
const size_t llm_model_count = sizeof(llm_models) / sizeof(llm_models[0]);

static llm_provider_t s_provider = LLM_PROVIDER_CLAUDE;
static bool s_ready = false;

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

esp_err_t llm_init(const char *model, const char *claude_key, const char *openai_key, const char *openai_base_url,
                   const char *system_prompt)
{
    s_provider = llm_provider_of(model);
    esp_err_t ret;
    if (s_provider == LLM_PROVIDER_OPENAI) {
        ret = openai_chat_init(openai_base_url, openai_key, model, system_prompt, effort_of(model));
    } else if (claude_key == NULL || claude_key[0] == '\0') {
        ESP_LOGW(TAG, "No Claude_key configured for %s", model);
        ret = ESP_ERR_INVALID_ARG;
    } else {
        ret = claude_client_init(claude_key, model, system_prompt, effort_of(model));
    }
    s_ready = (ret == ESP_OK);
    return ret;
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
    if (s_provider == LLM_PROVIDER_OPENAI) {
        return openai_chat_stream(text, on_text, ctx, full_text);
    }
    return claude_client_stream(text, on_text, ctx, full_text);
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
    if (s_provider == LLM_PROVIDER_OPENAI) {
        openai_chat_reset();
    } else {
        claude_client_reset();
    }
}
