/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Free models of OpenRouter, read from its live list (GET /api/v1/models, ~800 KB) at most once a day: those
 * whose prompt and completion prices are "0", answering text to text (no code or moderation model), the most
 * recent first, plus "openrouter/free" (OpenRouter picks a free model for each question). The list is kept on
 * SPIFFS, so that the models are offered at start-up and when OpenRouter cannot be reached.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "llm.h"
#include "openrouter_free.h"

#define FREE_MODELS_URL         "https://openrouter.ai/api/v1/models"
#define FREE_CACHE_FILE         "/spiffs/or_free.json"
#define FREE_REFRESH_S          (24 * 3600)
#define FREE_TIMEOUT_MS         20000
#define FREE_READ_CHUNK         1024
/* One model of the list, at most (the largest is ~4 KB) */
#define FREE_OBJECT_MAX         (12 * 1024)
/* The most recent free models offered, plus the automatic choice */
#define FREE_MODELS_KEPT        (LLM_OPENROUTER_FREE_MAX - 1)
#define FREE_AUTO_ID            "openrouter/free"

static const char *TAG = "openrouter_free";

/* When the list was last read (Unix time), 0 if never */
static time_t s_fetched = 0;

typedef struct {
    char *id;
    char *name;
    long long created;
} candidate_t;

/* Reading of the list: its models are parsed one at a time */
typedef struct {
    char *object;               /* the model being read (PSRAM) */
    size_t len;
    int depth;                  /* of '{' */
    bool in_string;
    bool escaped;
    bool overflow;
    candidate_t found[FREE_MODELS_KEPT];
    int found_count;
    int models;
} reader_t;

/* "NVIDIA: Nemotron 3.5 Lightning (free)" -> "Nemotron 3.5 Lightning" (allocated) */
static char *short_name(const char *name)
{
    const char *colon = strstr(name, ": ");
    char *s = strdup(colon ? colon + 2 : name);
    char *free_tag = s ? strstr(s, " (free)") : NULL;
    if (free_tag) {
        *free_tag = '\0';
    }
    return s;
}

static bool is_chat_model(const cJSON *m)
{
    const cJSON *pricing = cJSON_GetObjectItem(m, "pricing");
    const cJSON *prompt = cJSON_GetObjectItem(pricing, "prompt");
    const cJSON *completion = cJSON_GetObjectItem(pricing, "completion");
    const cJSON *id = cJSON_GetObjectItem(m, "id");
    if (!cJSON_IsString(prompt) || !cJSON_IsString(completion) || !cJSON_IsString(id) ||
            strcmp(prompt->valuestring, "0") != 0 || strcmp(completion->valuestring, "0") != 0 ||
            strcmp(id->valuestring, FREE_AUTO_ID) == 0) {
        return false;
    }
    // Text in, text only out (no music or images); no code or moderation model
    const cJSON *arch = cJSON_GetObjectItem(m, "architecture");
    const cJSON *out = cJSON_GetObjectItem(arch, "output_modalities");
    const cJSON *in = cJSON_GetObjectItem(arch, "input_modalities");
    bool text_in = false;
    const cJSON *mod;
    cJSON_ArrayForEach(mod, in) {
        text_in |= cJSON_IsString(mod) && strcmp(mod->valuestring, "text") == 0;
    }
    const cJSON *out0 = cJSON_GetArrayItem(out, 0);
    bool text_out = cJSON_GetArraySize(out) == 1 && cJSON_IsString(out0) && strcmp(out0->valuestring, "text") == 0;
    const char *i = id->valuestring;
    return text_in && text_out && !strstr(i, "code") && !strstr(i, "safety") && !strstr(i, "guard");
}

/* A whole model read: kept if free, among the most recent ones */
static void on_model(reader_t *r)
{
    r->models++;
    cJSON *m = cJSON_ParseWithLength(r->object, r->len);
    if (!m) {
        return;
    }
    if (is_chat_model(m)) {
        const cJSON *created = cJSON_GetObjectItem(m, "created");
        const cJSON *name = cJSON_GetObjectItem(m, "name");
        long long when = cJSON_IsNumber(created) ? (long long)created->valuedouble : 0;
        // Insertion among the kept ones, most recent first
        int pos = r->found_count;
        while (pos > 0 && r->found[pos - 1].created < when) {
            pos--;
        }
        if (pos < FREE_MODELS_KEPT) {
            if (r->found_count == FREE_MODELS_KEPT) {
                free(r->found[FREE_MODELS_KEPT - 1].id);
                free(r->found[FREE_MODELS_KEPT - 1].name);
                r->found_count--;
            }
            memmove(&r->found[pos + 1], &r->found[pos], (r->found_count - pos) * sizeof(candidate_t));
            r->found[pos].id = strdup(cJSON_GetObjectItem(m, "id")->valuestring);
            r->found[pos].name = short_name(cJSON_IsString(name) ? name->valuestring : r->found[pos].id);
            r->found[pos].created = when;
            r->found_count++;
        }
    }
    cJSON_Delete(m);
}

/* {"data":[{model},{model},...]}: each model (an object at depth 2) is cut out and parsed on its own */
static void reader_feed(reader_t *r, const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        if (r->depth >= 2) {
            if (r->len < FREE_OBJECT_MAX) {
                r->object[r->len++] = c;
            } else {
                r->overflow = true;
            }
        }
        if (r->in_string) {
            if (r->escaped) {
                r->escaped = false;
            } else if (c == '\\') {
                r->escaped = true;
            } else if (c == '"') {
                r->in_string = false;
            }
            continue;
        }
        if (c == '"') {
            r->in_string = true;
        } else if (c == '{') {
            if (++r->depth == 2) {
                r->len = 0;
                r->overflow = false;
                r->object[r->len++] = c;
            }
        } else if (c == '}') {
            if (r->depth-- == 2 && !r->overflow) {
                on_model(r);
            }
        }
    }
}

/* The models chosen, given to llm.c: the most recent free ones, then the automatic choice */
static void apply(const char *const *ids, const char *const *names, int n)
{
    llm_model_t models[LLM_OPENROUTER_FREE_MAX];
    char labels[LLM_OPENROUTER_FREE_MAX][64];
    int count = 0;
    for (int i = 0; i < n && count < LLM_OPENROUTER_FREE_MAX - 1; i++, count++) {
        snprintf(labels[count], sizeof(labels[count]), "%s (free)", names[i]);
        models[count] = (llm_model_t) { ids[i], labels[count], "low", names[i] };
    }
    models[count++] = (llm_model_t) { FREE_AUTO_ID, "Free (auto)", "low", "Free" };
    llm_set_openrouter_free_models(models, count);
    for (int i = 0; i < count; i++) {
        ESP_LOGI(TAG, "offered: %s (%s)", models[i].label, models[i].id);
    }
}

static void save_cache(reader_t *r)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "fetched", (double)s_fetched);
    cJSON *list = cJSON_AddArrayToObject(root, "models");
    for (int i = 0; i < r->found_count; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "id", r->found[i].id);
        cJSON_AddStringToObject(m, "name", r->found[i].name);
        cJSON_AddItemToArray(list, m);
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    FILE *f = text ? fopen(FREE_CACHE_FILE, "w") : NULL;
    if (f) {
        fputs(text, f);
        fclose(f);
    }
    free(text);
}

void openrouter_free_init(void)
{
    FILE *f = fopen(FREE_CACHE_FILE, "r");
    if (!f) {
        apply(NULL, NULL, 0);   // the automatic choice, until the list is read
        return;
    }
    char text[1024];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    cJSON *root = cJSON_Parse(text);
    const cJSON *fetched = cJSON_GetObjectItem(root, "fetched");
    s_fetched = cJSON_IsNumber(fetched) ? (time_t)fetched->valuedouble : 0;
    const char *ids[LLM_OPENROUTER_FREE_MAX];
    const char *names[LLM_OPENROUTER_FREE_MAX];
    int count = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(root, "models")) {
        const cJSON *id = cJSON_GetObjectItem(m, "id");
        const cJSON *name = cJSON_GetObjectItem(m, "name");
        if (cJSON_IsString(id) && cJSON_IsString(name) && count < LLM_OPENROUTER_FREE_MAX - 1) {
            ids[count] = id->valuestring;
            names[count++] = name->valuestring;
        }
    }
    apply(ids, names, count);   // the strings are copied
    cJSON_Delete(root);
    ESP_LOGI(TAG, "%d free models from the last list", count);
}

bool openrouter_free_refresh(void)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year + 1900 < 2024 || (s_fetched && now - s_fetched < FREE_REFRESH_S)) {
        return false;   // clock not set yet, or read less than a day ago
    }
    int64_t t0 = esp_timer_get_time();
    esp_http_client_config_t config = {
        .url = FREE_MODELS_URL,
        .timeout_ms = FREE_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    reader_t r = { .object = malloc(FREE_OBJECT_MAX) };
    char *chunk = malloc(FREE_READ_CHUNK);
    bool ok = false;
    if (client && r.object && chunk && esp_http_client_open(client, 0) == ESP_OK &&
            esp_http_client_fetch_headers(client) >= 0 && esp_http_client_get_status_code(client) == 200) {
        int n;
        while ((n = esp_http_client_read(client, chunk, FREE_READ_CHUNK)) > 0) {
            reader_feed(&r, chunk, n);
        }
        ok = (n == 0 && r.models > 0);
    }
    int status = client ? esp_http_client_get_status_code(client) : -1;
    if (client) {
        esp_http_client_cleanup(client);
    }
    free(chunk);
    free(r.object);
    if (ok) {
        s_fetched = now;
        const char *ids[FREE_MODELS_KEPT] = { 0 };
        const char *names[FREE_MODELS_KEPT] = { 0 };
        for (int i = 0; i < r.found_count; i++) {
            ids[i] = r.found[i].id;
            names[i] = r.found[i].name;
        }
        apply(ids, names, r.found_count);
        save_cache(&r);
        ESP_LOGI(TAG, "%d models read, %d free kept, in %lld ms", r.models, r.found_count,
                 (esp_timer_get_time() - t0) / 1000);
    } else {
        // Tried again in an hour (s_fetched set so that the next try waits)
        s_fetched = now - FREE_REFRESH_S + 3600;
        ESP_LOGW(TAG, "List not read (HTTP %d): the last known free models stay", status);
    }
    for (int i = 0; i < r.found_count; i++) {
        free(r.found[i].id);
        free(r.found[i].name);
    }
    return ok;
}
