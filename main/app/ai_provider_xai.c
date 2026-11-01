/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * xAI client (Grok models): its Responses API (ai_provider_responses.c), with xAI's web search and the device's
 * tools; a model refusing that API is asked through Chat Completions (ai_provider_chat_completions.c, the device's
 * tools only), until another model. Both keep the conversation as [{role, content}]: it goes from one to the other.
 */

#include "ai_provider_xai.h"
#include "ai_provider_chat_completions.h"
#include "ai_provider_responses.h"

static const rc_config_t s_rc_config = {
    .tag = "xai",
    .base_url = "https://api.x.ai/v1/",
    .search = RC_SEARCH_PLAIN,
    .web_search_cost = "xai:web_search",
};

/* A team without credit gets a 403 that says so */
static const char *const s_credit_words[] = { "spending limit", NULL };

static const cc_config_t s_cc_config = {
    .tag = "xai",
    .url = "https://api.x.ai/v1/chat/completions",
    // A tiny free answer (the key's details): opens the TLS connection in advance
    .prewarm_url = "https://api.x.ai/v1/api-key",
    .reasoning = CC_REASONING_EFFORT,
    .usage = CC_USAGE_STREAM_OPTIONS,
    .credit_words = s_credit_words,
    // Function calling: the device's tools (weather, radio, volume, Homey)
    .tools = true,
};

static rc_client_t s_rc = { .config = &s_rc_config };
static cc_client_t s_cc = { .config = &s_cc_config };
/* The model refused the Responses API: Chat Completions, until another model */
static bool s_chat_completions = false;

static esp_err_t xai_init(void *state, const char *api_key, const char *model, const char *instructions,
                          const char *effort)
{
    s_chat_completions = false;
    esp_err_t ret = rc_init(&s_rc, api_key, model, instructions, effort);
    if (ret == ESP_OK) {
        ret = cc_init(&s_cc, api_key, model, instructions, effort);
    }
    return ret;
}

static void xai_set_instructions(void *state, const char *instructions)
{
    rc_set_instructions(&s_rc, instructions);
    cc_set_instructions(&s_cc, instructions);
}

static void xai_reset(void *state)
{
    rc_reset(&s_rc);
    cc_reset(&s_cc);
}

static const cJSON *xai_history(void *state)
{
    return s_chat_completions ? cc_history(&s_cc) : rc_history(&s_rc);
}

static void xai_set_history(void *state, cJSON *messages)
{
    if (s_chat_completions) {
        cc_set_history(&s_cc, messages);
    } else {
        rc_set_history(&s_rc, messages);
    }
}

static void xai_prewarm(void *state)
{
    if (s_chat_completions) {
        cc_prewarm(&s_cc);
    } else {
        rc_prewarm(&s_rc);
    }
}

static void xai_set_tools(void *state, const cJSON *definitions, llm_tool_runner_t runner)
{
    rc_set_tools(&s_rc, definitions, runner);
    cc_set_tools(&s_cc, definitions, runner);
}

static llm_error_t xai_stream(void *state, const char *text, llm_text_cb_t on_text, void *ctx, char **full_text)
{
    if (!s_chat_completions) {
        llm_error_t err = rc_stream(&s_rc, text, on_text, ctx, full_text);
        if (!s_rc.api_refused) {
            return err;
        }
        // Chat Completions from now on, with the conversation so far
        s_chat_completions = true;
        cc_set_history(&s_cc, cJSON_Duplicate(rc_history(&s_rc), true));
    }
    return cc_stream(&s_cc, text, on_text, ctx, full_text);
}

const llm_backend_t llm_backend_xai = {
    .init = xai_init,
    .set_instructions = xai_set_instructions,
    .reset = xai_reset,
    .history = xai_history,
    .set_history = xai_set_history,
    .prewarm = xai_prewarm,
    .stream = xai_stream,
    .set_tools = xai_set_tools,
};
