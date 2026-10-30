/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * costs.json: {"since", "day", "month", "today": {model: entry}, "this_month": {...}, "total": {...},
 * "months": {"2026-10": [stt, llm, tts, tool, llm calls]}}, an entry being {"k": kind, "n": calls, "in", "read",
 * "write", "out" (tokens), "cost", "unpriced" (no price in AGENTS.INI)}. The tree lives in PSRAM (cJSON hooks).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "cJSON.h"
#include "file_lang.h"
#include "llm.h"
#include "file_costs.h"

#define COSTS_FILE          "/spiffs/costs.json"
#define COSTS_TMP_FILE      "/spiffs/costs.tmp"
#define COSTS_TEXT_FILE     "/spiffs/costs.txt"
#define COSTS_FILE_MAX      (24 * 1024)
#define MONTHS_KEPT         12
/* Valid once the year is past this one: the clock was synchronized */
#define SYNCED_MIN_YEAR     2024

/*
 * Price of the tokens read from / written to the cache, as a share of the price of the tokens read: Anthropic
 * 10 % / 125 %, OpenAI 10 %, Google 25 %, xAI 25 %, Mistral 10 %, Perplexity 25 %; OpenRouter depends on the
 * provider behind it (25 % taken)
 */
static const struct {
    double read;
    double write;
} s_cache_rates[LLM_PROVIDER_COUNT] = {
    [LLM_PROVIDER_CLAUDE] = { 0.10, 1.25 },
    [LLM_PROVIDER_OPENAI] = { 0.10, 1.00 },
    [LLM_PROVIDER_GEMINI] = { 0.25, 1.00 },
    [LLM_PROVIDER_OPENROUTER] = { 0.25, 1.00 },
    [LLM_PROVIDER_XAI] = { 0.25, 1.00 },
    [LLM_PROVIDER_MISTRAL] = { 0.10, 1.00 },
    [LLM_PROVIDER_PERPLEXITY] = { 0.25, 1.00 },
};

static const char *TAG = "costs";

static const char *const s_periods[] = { "today", "this_month", "total" };
static const char *const s_kind_names[COST_KINDS] = { "STT", "LLM", "TTS", "Tools" };

static SemaphoreHandle_t s_lock = NULL;
static cJSON *s_root = NULL;
static bool s_dirty = false;
static char s_currency[8] = "EUR";
/* Since the assistant last woke up: cost, tokens read and written */
static double s_conv[COST_KINDS];
static double s_conv_in[COST_KINDS];
static double s_conv_out[COST_KINDS];

/* ---------- dates ---------- */

/* "2026-10-27" and "2026-10", false while the clock is not synchronized */
static bool today(char day[11], char month[8])
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year + 1900 < SYNCED_MIN_YEAR) {
        return false;
    }
    strftime(day, 11, "%Y-%m-%d", &t);
    strftime(month, 8, "%Y-%m", &t);
    return true;
}

static const char *string_of(const cJSON *obj, const char *name, const char *fallback)
{
    const cJSON *item = cJSON_GetObjectItem(obj, name);
    return cJSON_IsString(item) ? item->valuestring : fallback;
}

static void set_string(cJSON *obj, const char *name, const char *value)
{
    cJSON_DeleteItemFromObject(obj, name);
    cJSON_AddStringToObject(obj, name, value);
}

static cJSON *object_of(cJSON *obj, const char *name)
{
    cJSON *item = cJSON_GetObjectItem(obj, name);
    if (!cJSON_IsObject(item)) {
        cJSON_DeleteItemFromObject(obj, name);
        item = cJSON_AddObjectToObject(obj, name);
    }
    return item;
}

/* A new day or month: its counters start again (the months table keeps the totals) */
static void roll_over(void)
{
    char day[11], month[8];
    if (!today(day, month)) {
        return;     // counted in the period known so far
    }
    if (strcmp(string_of(s_root, "since", ""), "") == 0) {
        set_string(s_root, "since", day);
    }
    if (strcmp(string_of(s_root, "day", ""), day) != 0) {
        cJSON_DeleteItemFromObject(s_root, "today");
        cJSON_AddObjectToObject(s_root, "today");
        set_string(s_root, "day", day);
        s_dirty = true;
    }
    if (strcmp(string_of(s_root, "month", ""), month) != 0) {
        cJSON_DeleteItemFromObject(s_root, "this_month");
        cJSON_AddObjectToObject(s_root, "this_month");
        set_string(s_root, "month", month);
        s_dirty = true;
    }
}

/* ---------- counting ---------- */

static void add_number(cJSON *obj, const char *name, double delta)
{
    cJSON *item = cJSON_GetObjectItem(obj, name);
    if (cJSON_IsNumber(item)) {
        cJSON_SetNumberValue(item, item->valuedouble + delta);
    } else {
        cJSON_DeleteItemFromObject(obj, name);
        cJSON_AddNumberToObject(obj, name, delta);
    }
}

static void add_entry(cJSON *period, const char *model, cost_kind_t kind, const int tokens[4], int calls,
                      double cost, bool priced)
{
    cJSON *entry = cJSON_GetObjectItem(period, model);
    if (!entry) {
        entry = cJSON_AddObjectToObject(period, model);
        cJSON_AddNumberToObject(entry, "k", kind);
    }
    add_number(entry, "n", calls);
    add_number(entry, "in", tokens[0]);
    add_number(entry, "read", tokens[1]);
    add_number(entry, "write", tokens[2]);
    add_number(entry, "out", tokens[3]);
    add_number(entry, "cost", cost);
    if (!priced && !cJSON_GetObjectItem(entry, "unpriced")) {
        cJSON_AddTrueToObject(entry, "unpriced");
    }
}

/* The month's line of the last MONTHS_KEPT months: [stt, llm, tts, tool, llm calls] */
static void add_month(cost_kind_t kind, int calls, double cost)
{
    const char *month = string_of(s_root, "month", "");
    if (!month[0]) {
        return;
    }
    cJSON *months = object_of(s_root, "months");
    cJSON *line = cJSON_GetObjectItem(months, month);
    if (!cJSON_IsArray(line)) {
        line = cJSON_AddArrayToObject(months, month);
        for (int i = 0; i <= COST_KINDS; i++) {
            cJSON_AddItemToArray(line, cJSON_CreateNumber(0));
        }
        // Only the last MONTHS_KEPT months: the oldest ("2025-01" < "2025-02") goes
        while (cJSON_GetArraySize(months) > MONTHS_KEPT) {
            const cJSON *oldest = NULL, *m;
            cJSON_ArrayForEach(m, months) {
                oldest = (!oldest || strcmp(m->string, oldest->string) < 0) ? m : oldest;
            }
            cJSON_DeleteItemFromObject(months, oldest->string);
        }
    }
    cJSON *value = cJSON_GetArrayItem(line, kind);
    cJSON_SetNumberValue(value, value->valuedouble + cost);
    if (kind == COST_LLM) {
        value = cJSON_GetArrayItem(line, COST_KINDS);
        cJSON_SetNumberValue(value, value->valuedouble + calls);
    }
}

static void add(cost_kind_t kind, const char *model, const int tokens[4], int calls, double cost, bool priced)
{
    if (!s_lock || !model) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_conv[kind] += cost;
    s_conv_in[kind] += tokens[0] + tokens[1] + tokens[2];
    s_conv_out[kind] += tokens[3];
    roll_over();
    for (size_t p = 0; p < sizeof(s_periods) / sizeof(s_periods[0]); p++) {
        add_entry(object_of(s_root, s_periods[p]), model, kind, tokens, calls, cost, priced);
    }
    add_month(kind, calls, cost);
    s_dirty = true;
    xSemaphoreGive(s_lock);
}

void costs_add_tokens(cost_kind_t kind, const char *model, int in, int cache_read, int cache_write, int out)
{
    double price_in = 0, price_out = 0;
    bool priced = model && llm_price_of(model, &price_in, &price_out);
    llm_provider_t provider = kind == COST_LLM ? llm_provider_of(model) : LLM_PROVIDER_OPENAI;
    double cost = (in * price_in + cache_read * price_in * s_cache_rates[provider].read +
                   cache_write * price_in * s_cache_rates[provider].write + out * price_out) / 1e6;
    const int tokens[4] = { in, cache_read, cache_write, out };
    add(kind, model, tokens, 1, cost, priced);
    // No float in the log: called from the WebSocket tasks too, whose stacks are small
    ESP_LOGI(TAG, "%s %s: in %d (cache read %d, write %d), out %d: %ld millionths of %s%s", s_kind_names[kind],
             model ? model : "-", in, cache_read, cache_write, out, (long)(cost * 1e6), s_currency,
             priced ? "" : " (no price)");
}

void costs_add_calls(const char *tool, int calls)
{
    if (calls <= 0) {
        return;
    }
    double price = 0;
    bool priced = llm_call_price_of(tool, &price);
    const int tokens[4] = { 0 };
    add(COST_TOOL, tool, tokens, calls, price * calls, priced);
    ESP_LOGI(TAG, "%s: %d call(s): %ld millionths of %s", tool, calls, (long)(price * calls * 1e6), s_currency);
}

void costs_conversation_start(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_conv, 0, sizeof(s_conv));
    memset(s_conv_in, 0, sizeof(s_conv_in));
    memset(s_conv_out, 0, sizeof(s_conv_out));
    xSemaphoreGive(s_lock);
}

double costs_conversation(double by_kind[COST_KINDS])
{
    double total = 0;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    for (int k = 0; k < COST_KINDS; k++) {
        if (by_kind) {
            by_kind[k] = s_conv[k];
        }
        total += s_conv[k];
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    return total;
}

double costs_period(cost_period_t period, const char *model, double by_kind[COST_KINDS])
{
    double total = 0;
    for (int k = 0; by_kind && k < COST_KINDS; k++) {
        by_kind[k] = 0;
    }
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    roll_over();
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(s_root, s_periods[period])) {
        const cJSON *k = cJSON_GetObjectItem(e, "k");
        const cJSON *cost = cJSON_GetObjectItem(e, "cost");
        if ((model && strcmp(e->string, model) != 0) || !cJSON_IsNumber(k) || !cJSON_IsNumber(cost) ||
                k->valueint < 0 || k->valueint >= COST_KINDS) {
            continue;
        }
        if (by_kind) {
            by_kind[k->valueint] += cost->valuedouble;
        }
        total += cost->valuedouble;
    }
    xSemaphoreGive(s_lock);
    return total;
}

static double number_in(const cJSON *obj, const char *name)
{
    const cJSON *item = cJSON_GetObjectItem(obj, name);
    return cJSON_IsNumber(item) ? item->valuedouble : 0;
}

void costs_detail(cost_scope_t scope, cost_detail_t by_kind[COST_KINDS])
{
    memset(by_kind, 0, COST_KINDS * sizeof(cost_detail_t));
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (scope == COST_SCOPE_CONVERSATION) {
        for (int k = 0; k < COST_KINDS; k++) {
            by_kind[k] = (cost_detail_t) { s_conv[k], s_conv_in[k], s_conv_out[k] };
        }
    } else {
        roll_over();
        const cJSON *e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItem(s_root, s_periods[scope == COST_SCOPE_TODAY ? COST_PERIOD_TODAY :
                                                                       COST_PERIOD_MONTH])) {
            const cJSON *k = cJSON_GetObjectItem(e, "k");
            if (!cJSON_IsNumber(k) || k->valueint < 0 || k->valueint >= COST_KINDS) {
                continue;
            }
            cost_detail_t *d = &by_kind[k->valueint];
            d->cost += number_in(e, "cost");
            d->in += number_in(e, "in") + number_in(e, "read") + number_in(e, "write");
            d->out += number_in(e, "out");
        }
    }
    xSemaphoreGive(s_lock);
}

/* ---------- formatting ---------- */

void costs_format_tokens(char *buf, size_t size, double tokens)
{
    if (tokens < 1000) {
        snprintf(buf, size, "%.0f", tokens);
    } else if (tokens < 1e6) {
        snprintf(buf, size, "%.1f k", tokens / 1e3);
    } else {
        snprintf(buf, size, "%.2f M", tokens / 1e6);
    }
    char mark = app_lang_get()->decimal_mark;
    for (char *c = buf; *c; c++) {
        *c = (*c == '.') ? mark : *c;
    }
}

/* "0,0123" with the decimal mark of the language; 4 decimals under 1, 2 above (one less with fewer) */
static void amount_text_ex(char *buf, size_t size, double amount, int fewer)
{
    snprintf(buf, size, "%.*f", (amount < 1 ? 4 : 2) - fewer, amount);
    char mark = app_lang_get()->decimal_mark;
    for (char *c = buf; *c; c++) {
        *c = (*c == '.') ? mark : *c;
    }
}

static void amount_text(char *buf, size_t size, double amount)
{
    amount_text_ex(buf, size, amount, 0);
}

static void format_amount(char *buf, size_t size, double amount, bool with_currency, int fewer)
{
    char number[24];
    amount_text_ex(number, sizeof(number), amount, fewer);
    if (!with_currency) {
        strlcpy(buf, number, size);
    } else if (strcasecmp(s_currency, "EUR") == 0) {
        snprintf(buf, size, "%s €", number);
    } else if (strcasecmp(s_currency, "USD") == 0) {
        snprintf(buf, size, "$%s", number);
    } else {
        snprintf(buf, size, "%s %s", number, s_currency);
    }
}

void costs_format(char *buf, size_t size, double amount, bool with_currency)
{
    format_amount(buf, size, amount, with_currency, 0);
}

void costs_format_short(char *buf, size_t size, double amount, bool with_currency)
{
    format_amount(buf, size, amount, with_currency, 1);
}

/* ---------- COSTS.TXT ---------- */

typedef struct {
    const char *intro;
    const char *today;
    const char *month;
    const char *total;
    const char *months;
    const char *columns;
    const char *month_columns;
    const char *sum;
} costs_text_t;

static const costs_text_t s_text_fr = {
    "# Coûts de l'assistant vocal, monnaie : %s (prix d'AGENTS.INI, par million de tokens ou par appel).\r\n"
    "# Mis à jour à chaque mise en veille. in : tokens lus (entre parenthèses : lus ou écrits en cache, moins\r\n"
    "# chers) ; out : tokens écrits. STT = transcription, LLM = réponse, TTS = voix, Outils = appels payés à\r\n"
    "# l'unité (recherche web). ? : pas de prix dans AGENTS.INI. Fichier réécrit : ne pas le modifier.\r\n",
    "Aujourd'hui", "Ce mois", "Depuis le début", "12 derniers mois",
    "modèle                          type   appels             in (cache)          out        coût",
    "mois          STT        LLM        TTS     Outils      total  appels LLM",
    "total",
};

static const costs_text_t s_text_en = {
    "# Costs of the voice assistant, currency: %s (prices of AGENTS.INI, per million tokens or per call).\r\n"
    "# Updated each time it goes to sleep. in: tokens read (in brackets: read from or written to the cache,\r\n"
    "# cheaper); out: tokens written. STT = transcription, LLM = answer, TTS = speech, Tools = calls billed per use\r\n"
    "# (web search). ?: no price in AGENTS.INI. Rewritten file: do not edit it.\r\n",
    "Today", "This month", "Since the start", "Last 12 months",
    "model                           type    calls             in (cache)          out        cost",
    "month         STT        LLM        TTS      Tools      total   LLM calls",
    "total",
};

static double number_of(const cJSON *obj, const char *name)
{
    const cJSON *item = cJSON_GetObjectItem(obj, name);
    return cJSON_IsNumber(item) ? item->valuedouble : 0;
}

static void write_period(FILE *f, const costs_text_t *tx, const char *title, const char *when, const cJSON *period)
{
    fprintf(f, "\r\n== %s (%s) ==\r\n%s\r\n", title, when, tx->columns);
    double sums[COST_KINDS] = { 0 };
    double total = 0;
    char cost[24], in[40];
    // By kind (STT, LLM, TTS, tools), in the order they are in
    for (int kind = 0; kind < COST_KINDS; kind++) {
        const cJSON *e;
        cJSON_ArrayForEach(e, period) {
            if ((int)number_of(e, "k") != kind) {
                continue;
            }
            double c = number_of(e, "cost");
            sums[kind] += c;
            total += c;
            amount_text(cost, sizeof(cost), c);
            long long cached = (long long)(number_of(e, "read") + number_of(e, "write"));
            if (kind == COST_TOOL) {
                strlcpy(in, "-", sizeof(in));
            } else {
                snprintf(in, sizeof(in), "%lld (%lld)", (long long)number_of(e, "in") + cached, cached);
            }
            char out[24];
            snprintf(out, sizeof(out), "%lld", (long long)number_of(e, "out"));
            fprintf(f, "%-31.31s %-6s %7lld %22s %12s %11s%s\r\n", e->string,
                    kind == COST_TOOL && app_lang_get()->decimal_mark == ',' ? "Outils" : s_kind_names[kind],
                    (long long)number_of(e, "n"), in, kind == COST_TOOL ? "-" : out, cost,
                    cJSON_IsTrue(cJSON_GetObjectItem(e, "unpriced")) ? " ?" : "");
        }
    }
    char s[COST_KINDS][24];
    for (int k = 0; k < COST_KINDS; k++) {
        amount_text(s[k], sizeof(s[k]), sums[k]);
    }
    amount_text(cost, sizeof(cost), total);
    fprintf(f, "%93s %s\r\n  STT %s · LLM %s · TTS %s · %s %s\r\n", tx->sum, cost, s[COST_STT], s[COST_LLM],
            s[COST_TTS], app_lang_get()->decimal_mark == ',' ? "Outils" : "Tools", s[COST_TOOL]);
}

static int compare_desc(const void *a, const void *b)
{
    return strcmp((*(const cJSON * const *)b)->string, (*(const cJSON * const *)a)->string);
}

static void write_text_file(void)
{
    FILE *f = fopen(COSTS_TEXT_FILE, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot write %s", COSTS_TEXT_FILE);
        return;
    }
    const costs_text_t *tx = strcmp(app_lang_get()->code, "fr") == 0 ? &s_text_fr : &s_text_en;
    fprintf(f, tx->intro, s_currency);
    write_period(f, tx, tx->today, string_of(s_root, "day", "?"), cJSON_GetObjectItem(s_root, "today"));
    write_period(f, tx, tx->month, string_of(s_root, "month", "?"), cJSON_GetObjectItem(s_root, "this_month"));
    write_period(f, tx, tx->total, string_of(s_root, "since", "?"), cJSON_GetObjectItem(s_root, "total"));

    fprintf(f, "\r\n== %s ==\r\n%s\r\n", tx->months, tx->month_columns);
    const cJSON *months = cJSON_GetObjectItem(s_root, "months");
    const cJSON *sorted[MONTHS_KEPT];
    int n = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, months) {
        if (n < MONTHS_KEPT) {
            sorted[n++] = m;
        }
    }
    qsort(sorted, n, sizeof(sorted[0]), compare_desc);
    for (int i = 0; i < n; i++) {
        char v[COST_KINDS + 1][24];
        double total = 0;
        for (int k = 0; k < COST_KINDS; k++) {
            double c = cJSON_GetArrayItem(sorted[i], k) ? cJSON_GetArrayItem(sorted[i], k)->valuedouble : 0;
            total += c;
            amount_text(v[k], sizeof(v[k]), c);
        }
        amount_text(v[COST_KINDS], sizeof(v[COST_KINDS]), total);
        const cJSON *calls = cJSON_GetArrayItem(sorted[i], COST_KINDS);
        fprintf(f, "%-9s %10s %10s %10s %10s %10s %11d\r\n", sorted[i]->string, v[COST_STT], v[COST_LLM],
                v[COST_TTS], v[COST_TOOL], v[COST_KINDS], calls ? (int)calls->valuedouble : 0);
    }
    fclose(f);
}

/* ---------- saving ---------- */

void costs_save(void)
{
    if (!s_lock || !s_dirty) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    roll_over();
    char *text = cJSON_PrintUnformatted(s_root);
    write_text_file();
    s_dirty = false;
    xSemaphoreGive(s_lock);
    // Written to a temporary file first: a power cut during the write keeps the previous costs
    FILE *f = text ? fopen(COSTS_TMP_FILE, "w") : NULL;
    bool ok = f && fputs(text, f) >= 0;
    if (f) {
        ok &= (fclose(f) == 0);
    }
    if (ok) {
        remove(COSTS_FILE);
        ok = (rename(COSTS_TMP_FILE, COSTS_FILE) == 0);
    }
    ESP_LOGI(TAG, "%s (%u bytes)", ok ? "Costs saved" : "Could not save the costs", text ? (unsigned)strlen(text) : 0);
    free(text);
}

void costs_init(const char *currency)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    strlcpy(s_currency, currency && currency[0] ? currency : "EUR", sizeof(s_currency));
    FILE *f = fopen(COSTS_FILE, "r");
    char *text = f ? malloc(COSTS_FILE_MAX) : NULL;
    size_t n = text ? fread(text, 1, COSTS_FILE_MAX - 1, f) : 0;
    if (f) {
        fclose(f);
    }
    if (text) {
        text[n] = '\0';
        s_root = cJSON_Parse(text);
        free(text);
    }
    if (!cJSON_IsObject(s_root)) {
        cJSON_Delete(s_root);
        s_root = cJSON_CreateObject();
    }
    const cJSON *total = cJSON_GetObjectItem(s_root, "total");
    ESP_LOGI(TAG, "%d model(s) counted since %s, currency %s", cJSON_GetArraySize(total),
             string_of(s_root, "since", "now"), s_currency);
}
