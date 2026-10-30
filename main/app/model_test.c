/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Model test: the time to the first words of each model (what is heard), its mean over the tests, the time to
 * the end of the answer and its tokens. Kept by model id in MODEL_TIMES_FILE, shown in SPEED.TXT.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cJSON.h"
#include "file_lang.h"
#include "assistant.h"
#include "llm.h"
#include "ai_provider_openrouter_free.h"
#include "voice_cmd.h"
#include "app_ui_ctrl.h"
#include "model_test.h"
#include "settings.h"
#include "stt_stream.h"
#include "tts_stream.h"
#include "file_memory.h"

#define MODEL_TIMES_FILE        "/spiffs/model_times.json"
#define MODEL_TIMES_FILE_MAX    8192
#define SPEED_FILE              "/spiffs/speed.txt"
/* Its name before (LOG.TXT): removed by the next test */
#define OLD_SPEED_FILE          "/spiffs/log.txt"
/* The list of the results stays on screen this long after the end */
#define MODEL_TEST_LIST_SHOWN_MS    15000
/* Free OpenRouter models slower than this on average: asked whether to exclude them */
#define MODEL_TEST_SLOW_FREE_MS     5000

static const char *TAG = "model_test";

static volatile bool s_running = false;
static volatile bool s_pending = false;

/* The slow free models asked about after the test, and the next one */
typedef struct {
    char id[80];
    char name[48];
    int avg_ms;
} slow_model_t;
/* PSRAM (internal RAM is scarce): LLM_OPENROUTER_FREE_MAX entries, allocated by the first test */
static slow_model_t *s_slow = NULL;
static int s_slow_count = 0;
static volatile int s_slow_next = 0;
/* An exclusion was added: the free models offered are chosen again (main task) */
static volatile bool s_free_reload = false;
static volatile bool s_cancel = false;

bool model_test_running(void)
{
    return s_running;
}

void model_test_cancel(void)
{
    if (s_running && !s_cancel) {
        ESP_LOGI(TAG, "Stopped after the model being tested");
        s_cancel = true;
    }
}

bool model_test_cancelled(void)
{
    return s_cancel;
}

/* ---------- results, by model id ---------- */

static cJSON *load_times(void)
{
    FILE *f = fopen(MODEL_TIMES_FILE, "r");
    char *text = f ? malloc(MODEL_TIMES_FILE_MAX) : NULL;
    size_t n = text ? fread(text, 1, MODEL_TIMES_FILE_MAX - 1, f) : 0;
    if (f) {
        fclose(f);
    }
    cJSON *times = NULL;
    if (text) {
        text[n] = '\0';
        times = cJSON_Parse(text);
        free(text);
    }
    if (!cJSON_IsObject(times)) {
        cJSON_Delete(times);
        times = cJSON_CreateObject();
    }
    return times;
}

static void save_times(const cJSON *times)
{
    char *text = cJSON_PrintUnformatted(times);
    FILE *f = text ? fopen(MODEL_TIMES_FILE, "w") : NULL;
    if (f) {
        fputs(text, f);
        fclose(f);
    }
    free(text);
}

static void set_number(cJSON *entry, const char *name, double value)
{
    cJSON *item = cJSON_GetObjectItem(entry, name);
    if (item) {
        cJSON_SetNumberValue(item, value);
    } else {
        cJSON_AddNumberToObject(entry, name, value);
    }
}

static void set_string(cJSON *entry, const char *name, const char *value)
{
    cJSON_DeleteItemFromObject(entry, name);
    cJSON_AddStringToObject(entry, name, value ? value : "");
}

static double number_of(const cJSON *entry, const char *name)
{
    const cJSON *item = cJSON_GetObjectItem(entry, name);
    return cJSON_IsNumber(item) ? item->valuedouble : 0;
}

static const char *string_of(const cJSON *entry, const char *name)
{
    const cJSON *item = cJSON_GetObjectItem(entry, name);
    return cJSON_IsString(item) ? item->valuestring : "";
}

static const char *status_of(llm_error_t err)
{
    switch (err) {
    case LLM_OK: return "ok";
    case LLM_ERR_NO_KEY: return "unavailable: no key";
    case LLM_ERR_NETWORK: return "unavailable: no answer";
    case LLM_ERR_AUTH: return "unavailable: key refused";
    case LLM_ERR_CREDIT: return "unavailable: no credit";
    case LLM_ERR_BUSY: return "unavailable: busy";
    case LLM_ERR_REFUSAL: return "unavailable: refused";
    case LLM_ERR_EFFORT: return "unavailable: reasoning effort refused (AGENTS.INI)";
    default: return "unavailable: error";
    }
}

/* The result of a model; its mean time to the first words counts every successful test */
static void record(cJSON *times, const llm_model_t *m, llm_error_t err, const llm_stats_t *st)
{
    cJSON *entry = cJSON_GetObjectItem(times, m->id);
    if (!entry) {
        entry = cJSON_AddObjectToObject(times, m->id);
    }
    char date[20];
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &t);
    set_string(entry, "name", m->spoken);
    set_string(entry, "date", date);
    bool ok = (err == LLM_OK && st->first_ms >= 0);
    set_string(entry, "status", ok ? "ok" : status_of(err == LLM_OK ? LLM_ERR_OTHER : err));
    if (!ok) {
        return;
    }
    int n = (int)number_of(entry, "n") + 1;
    double avg = number_of(entry, "avg");
    set_number(entry, "n", n);
    set_number(entry, "avg", avg + (st->first_ms - avg) / n);
    set_number(entry, "first", st->first_ms);
    set_number(entry, "total", st->total_ms);
    set_number(entry, "tokens", st->answer_tokens);
    set_number(entry, "reasoning", st->reasoning_tokens);
}

static void spoken_seconds(char *buf, size_t size, int ms, bool short_unit);

/* ---------- the speech models ([tts], [stt] of AGENTS.INI) ---------- */

/*
 * The sentence of LANG_<XX>.TXT (speech_test_phrase) spoken by each speech model into memory (not played), then
 * the audio of the first one transcribed by each transcription model. Shown in SPEED.TXT after the chat models.
 */
#define SPEECH_RESULTS_MAX  16
static struct {
    bool stt;
    char name[48];
    char model[48];
    int first_ms;           /* TTS: first audio; STT: the transcript after all the audio is sent */
    int total_ms;           /* TTS: end of the audio */
    int words;              /* STT: % of the words of the sentence found in the transcript, -1 if none */
    char status[32];
} s_speech_results[SPEECH_RESULTS_MAX];
static int s_speech_result_count = 0;

/* The key of a speech model's provider, NULL without one */
static const char *speech_key(const llm_speech_model_t *m)
{
    const sys_param_t *param = settings_get_parameter();
    const char *key = strcmp(m->provider, "mistral") == 0 ? param->mistral_key : param->key;
    return key[0] ? key : NULL;
}

/* % of the words of the sentence (3 letters or more) found in the transcript */
static int words_found(const char *sentence, const char *transcript)
{
    // Each one between spaces, so that a word is not found inside another
    char said[256], heard[512] = " ";
    app_text_normalize(sentence, said, sizeof(said));
    app_text_normalize(transcript, heard + 1, sizeof(heard) - 2);
    strlcat(heard, " ", sizeof(heard));
    int total = 0, found = 0;
    for (char *w = strtok(said, " "); w; w = strtok(NULL, " ")) {
        if (strlen(w) < 3) {
            continue;
        }
        total++;
        char pattern[sizeof(said) + 2] = " ";
        strlcat(pattern, w, sizeof(pattern));
        strlcat(pattern, " ", sizeof(pattern));
        found += strstr(heard, pattern) != NULL;
    }
    return total ? found * 100 / total : -1;
}

static void speech_line(char *screen, size_t size, const char *name, const char *result)
{
    size_t from = strlen(screen);
    snprintf(screen + from, size - from, "%s%s%s%s", from ? "\n" : "", name, result ? " : " : "…", result ? result : "");
}

/*
 * The result of a speech model, said at once and waited for: "Résultat de la synthèse Voxtral : 0,7 seconde."
 * (the next speech test would cut it). A synthesis is said with its own voice, still set up after its test.
 */
static void say_speech_result(int index)
{
    const app_lang_t *lang = app_lang_get();
    char text[160], time_text[32];
    if (s_speech_results[index].first_ms >= 0) {
        spoken_seconds(time_text, sizeof(time_text), s_speech_results[index].first_ms, false);
        snprintf(text, sizeof(text), lang->test_result, s_speech_results[index].name, time_text);
    } else {
        snprintf(text, sizeof(text), lang->test_unavailable, s_speech_results[index].name);
        if ((unsigned char)text[0] < 0x80) {
            text[0] = toupper((unsigned char)text[0]);     // "La synthèse Voxtral : non disponible."
        }
    }
    assistant_announce_begin();
    assistant_announce(text);
    assistant_announce_end();
}

static void test_speech(char *screen, size_t screen_size)
{
    const app_lang_t *lang = app_lang_get();
    const sys_param_t *param = settings_get_parameter();
    int16_t *audio = NULL;
    size_t samples = 0;
    char name[32], result[32];
    s_speech_result_count = 0;
    // "Je teste les 30 modèles" said to the end first (a speech test would cut it)
    for (int i = 0; i < 300 && !tts_stream_is_silent(500); i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    for (size_t i = 0; i < llm_speech_count(LLM_SPEECH_TTS) && !s_cancel && s_speech_result_count < SPEECH_RESULTS_MAX; i++) {
        const llm_speech_model_t *m = llm_speech_model(LLM_SPEECH_TTS, i);
        bool mistral = strcmp(m->provider, "mistral") == 0;
        snprintf(name, sizeof(name), lang->test_tts_name, m->spoken);
        size_t line_start = strlen(screen);
        speech_line(screen, screen_size, name, NULL);
        assistant_show(screen);
        typeof(s_speech_results[0]) *r = &s_speech_results[s_speech_result_count++];
        *r = (typeof(*r)) { .stt = false, .first_ms = -1, .total_ms = -1, .words = -1 };
        strlcpy(r->name, name, sizeof(r->name));
        strlcpy(r->model, m->id, sizeof(r->model));
        const char *key = speech_key(m);
        const llm_voice_t *v = llm_voice_for(m->provider, lang->code, param->voice);
        int16_t *got = NULL;
        size_t got_samples = 0;
        bool ok = key && tts_stream_test(mistral ? &tts_backend_mistral : &tts_backend_openai, param->url, key, m->id,
                                         v ? v->id : NULL, lang->speech_test_phrase,
                                         &got, &got_samples, &r->first_ms, &r->total_ms);
        strlcpy(r->status, !key ? "unavailable: no key" : ok ? "ok" : "unavailable: no audio", sizeof(r->status));
        ESP_LOGI(TAG, "TTS %s: %s, first audio %d ms, total %d ms, %u samples", m->id, r->status, r->first_ms,
                 r->total_ms, (unsigned)got_samples);
        if (ok && !audio) {
            audio = got;        // the audio transcribed next
            samples = got_samples;
        } else {
            free(got);
        }
        screen[line_start] = '\0';
        if (ok) {
            spoken_seconds(result, sizeof(result), r->first_ms, true);
        }
        speech_line(screen, screen_size, name, ok ? result : lang->test_unavailable_short);
        assistant_show(screen);
        if (!s_cancel) {
            say_speech_result(s_speech_result_count - 1);   // with the voice just tested
        }
    }
    // The speech of the settings again
    assistant_set_tts(NULL);

    for (size_t i = 0; i < llm_speech_count(LLM_SPEECH_STT) && audio && !s_cancel &&
            s_speech_result_count < SPEECH_RESULTS_MAX; i++) {
        const llm_speech_model_t *m = llm_speech_model(LLM_SPEECH_STT, i);
        bool mistral = strcmp(m->provider, "mistral") == 0;
        snprintf(name, sizeof(name), lang->test_stt_name, m->spoken);
        size_t line_start = strlen(screen);
        speech_line(screen, screen_size, name, NULL);
        assistant_show(screen);
        typeof(s_speech_results[0]) *r = &s_speech_results[s_speech_result_count++];
        *r = (typeof(*r)) { .stt = true, .first_ms = -1, .total_ms = -1, .words = -1 };
        strlcpy(r->name, name, sizeof(r->name));
        strlcpy(r->model, m->id, sizeof(r->model));
        const char *key = speech_key(m);
        char *text = NULL;
        if (key) {
            stt_stream_configure(mistral ? &stt_backend_mistral : &stt_backend_openai, key, m->id, lang->code);
            text = stt_stream_test(audio, samples, &r->first_ms);
        }
        r->words = text ? words_found(lang->speech_test_phrase, text) : -1;
        strlcpy(r->status, !key ? "unavailable: no key" : text ? "ok" : "unavailable: no text", sizeof(r->status));
        ESP_LOGI(TAG, "STT %s: %s, %d ms, %d %% of the words: \"%s\"", m->id, r->status, r->first_ms, r->words,
                 text ? text : "");
        free(text);
        screen[line_start] = '\0';
        if (r->first_ms >= 0) {
            spoken_seconds(result, sizeof(result), r->first_ms, true);
        }
        speech_line(screen, screen_size, name, r->first_ms >= 0 ? result : lang->test_unavailable_short);
        assistant_show(screen);
        if (!s_cancel) {
            say_speech_result(s_speech_result_count - 1);
        }
    }
    free(audio);
    // The transcription of the settings again
    assistant_set_stt(NULL);
}

/* ---------- SPEED.TXT ---------- */

static int compare_avg(const void *a, const void *b)
{
    const cJSON *ea = *(const cJSON * const *)a;
    const cJSON *eb = *(const cJSON * const *)b;
    double va = number_of(ea, "n") > 0 ? number_of(ea, "avg") : 1e12;
    double vb = number_of(eb, "n") > 0 ? number_of(eb, "avg") : 1e12;
    return va < vb ? -1 : va > vb;
}

/* Seconds with one decimal ("1.2"), "-" when unknown */
static const char *secs(char *buf, size_t size, double ms, bool known)
{
    if (!known) {
        return "-";
    }
    snprintf(buf, size, "%.1f", ms / 1000.0);
    return buf;
}

static void write_speed_file(const cJSON *times, const char *question)
{
    int count = cJSON_GetArraySize(times);
    const cJSON **entries = calloc(count ? count : 1, sizeof(*entries));
    remove(OLD_SPEED_FILE);
    FILE *f = entries ? fopen(SPEED_FILE, "w") : NULL;
    if (!f) {
        free(entries);
        ESP_LOGE(TAG, "Cannot write %s", SPEED_FILE);
        return;
    }
    int i = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, times) {
        entries[i++] = e;
    }
    qsort(entries, count, sizeof(*entries), compare_avg);

    fprintf(f, "# Model test (\"teste les modeles\"): same question to every model, without the conversation\r\n"
            "# Question: \"%s\"\r\n"
            "# first: time to the first words of the answer (what you hear), in seconds; avg: its mean over n tests\r\n"
            "# total: time to the end of the answer; tokens: tokens of the answer (reason: of the reasoning, - when\r\n"
            "# not given apart); tok/s: tokens of the answer per second after its first words\r\n"
            "# speed: written to AGENTS.INI (models of AGENTS.INI only), from the avg of the fastest and slowest\r\n"
            "# Lines sorted by avg. This file is rewritten by each test.\r\n\r\n", question);
    fprintf(f, "%-24s %6s %6s %3s %6s %6s %6s %6s  %-7s  %-16s  %-26s  %s\r\n", "name", "first", "avg", "n",
            "total", "tokens", "reason", "tok/s", "speed", "tested", "status", "model");
    for (i = 0; i < count; i++) {
        e = entries[i];
        bool tested = number_of(e, "n") > 0;
        double first = number_of(e, "first");
        double total = number_of(e, "total");
        int tokens = (int)number_of(e, "tokens");
        int reasoning = (int)number_of(e, "reasoning");
        char b1[12], b2[12], b3[12], reason[12], tps[12], n[8], tok[12];
        snprintf(n, sizeof(n), "%d", (int)number_of(e, "n"));
        snprintf(tok, sizeof(tok), "%d", tokens);
        snprintf(reason, sizeof(reason), "%d", reasoning);
        double gen_s = (total - first) / 1000.0;
        snprintf(tps, sizeof(tps), "%.0f", gen_s > 0.05 ? tokens / gen_s : 0.0);
        fprintf(f, "%-24.24s %6s %6s %3s %6s %6s %6s %6s  %-7s  %-16s  %-26s  %s\r\n", string_of(e, "name"),
                secs(b1, sizeof(b1), first, tested), secs(b2, sizeof(b2), number_of(e, "avg"), tested),
                tested ? n : "0", secs(b3, sizeof(b3), total, tested), tested ? tok : "-",
                tested && reasoning >= 0 ? reason : "-", tested && gen_s > 0.05 ? tps : "-",
                string_of(e, "speed")[0] ? string_of(e, "speed") : "-", string_of(e, "date"),
                string_of(e, "status"), e->string);
    }
    if (s_speech_result_count) {
        fprintf(f, "\r\n# Speech models of the last test: speech_test_phrase of LANG_<XX>.TXT spoken by each speech model\r\n"
                "# (TTS, not played: first = first audio, total = end of the audio), then the audio of the first one\r\n"
                "# transcribed by each transcription model (STT: first = transcript after all the audio sent,\r\n"
                "# words = %% of the words of the sentence found)\r\n\r\n");
        fprintf(f, "%-32s %6s %6s %6s  %-26s  %s\r\n", "name", "first", "total", "words", "status", "model");
        for (i = 0; i < s_speech_result_count; i++) {
            char b1[12], b2[12], w[8];
            bool ok = s_speech_results[i].first_ms >= 0;
            snprintf(w, sizeof(w), "%d", s_speech_results[i].words);
            fprintf(f, "%-32.32s %6s %6s %6s  %-26s  %s\r\n", s_speech_results[i].name,
                    secs(b1, sizeof(b1), s_speech_results[i].first_ms, ok),
                    secs(b2, sizeof(b2), s_speech_results[i].total_ms, ok && !s_speech_results[i].stt),
                    s_speech_results[i].words >= 0 ? w : "-", s_speech_results[i].status, s_speech_results[i].model);
        }
    }
    fclose(f);
    free(entries);
}

/* ---------- speed of the models of AGENTS.INI ---------- */

/* fast: in the third of the range (between the fastest and the slowest mean) nearest the fastest; slow: nearest
 * the slowest; average otherwise. Models never answered keep their speed. */
static void classify(cJSON *times)
{
    size_t count = llm_agents_count();
    double min = 0, max = 0;
    bool any = false;
    for (size_t i = 0; i < count; i++) {
        const cJSON *e = cJSON_GetObjectItem(times, llm_models[i].id);
        if (number_of(e, "n") > 0) {
            double avg = number_of(e, "avg");
            min = (!any || avg < min) ? avg : min;
            max = (!any || avg > max) ? avg : max;
            any = true;
        }
    }
    if (!any) {
        return;
    }
    double third = (max - min) / 3;
    const char *speeds[LLM_FILE_MODELS_MAX] = { NULL };
    for (size_t i = 0; i < count; i++) {
        cJSON *e = cJSON_GetObjectItem(times, llm_models[i].id);
        if (number_of(e, "n") <= 0) {
            continue;
        }
        double avg = number_of(e, "avg");
        speeds[i] = avg <= min + third ? "fast" : (avg >= max - third ? "slow" : "average");
        llm_models[i].speed = speeds[i];
        set_string(e, "speed", speeds[i]);
        ESP_LOGI(TAG, "%s: %s (%.0f ms)", llm_models[i].id, speeds[i], avg);
    }
    llm_save_speeds(speeds);
}

/* ---------- slow free models: exclude them? ---------- */

static void find_slow_free(const cJSON *times)
{
    s_slow_count = 0;
    s_slow_next = 0;
    if (!s_slow && !(s_slow = heap_caps_calloc(LLM_OPENROUTER_FREE_MAX, sizeof(slow_model_t), MALLOC_CAP_SPIRAM))) {
        return;
    }
    for (size_t i = llm_agents_count(); i < llm_model_count && s_slow_count < LLM_OPENROUTER_FREE_MAX; i++) {
        const cJSON *e = cJSON_GetObjectItem(times, llm_models[i].id);
        if (number_of(e, "n") > 0 && number_of(e, "avg") > MODEL_TEST_SLOW_FREE_MS) {
            slow_model_t *s = &s_slow[s_slow_count++];
            strlcpy(s->id, llm_models[i].id, sizeof(s->id));
            strlcpy(s->name, llm_models[i].spoken, sizeof(s->name));
            s->avg_ms = (int)number_of(e, "avg");
        }
    }
    ESP_LOGI(TAG, "%d free model(s) slower than %d ms on average", s_slow_count, MODEL_TEST_SLOW_FREE_MS);
}

static void spoken_seconds(char *buf, size_t size, int ms, bool short_unit);

bool model_test_exclusion_question(char *buf, size_t size)
{
    if (s_slow_next >= s_slow_count) {
        return false;
    }
    const slow_model_t *s = &s_slow[s_slow_next];
    char time_text[32];
    spoken_seconds(time_text, sizeof(time_text), s->avg_ms, false);
    snprintf(buf, size, app_lang_get()->exclude_ask, s->name, time_text);
    return true;
}

void model_test_exclusion_answer(bool yes)
{
    if (s_slow_next >= s_slow_count) {
        return;
    }
    const slow_model_t *s = &s_slow[s_slow_next++];
    if (yes) {
        char comment[48];
        snprintf(comment, sizeof(comment), "%.1f s on average (model test)", s->avg_ms / 1000.0);
        llm_free_exclude_add(s->id, comment);
        s_free_reload = true;
    }
    ESP_LOGI(TAG, "%s: %s", s->id, yes ? "excluded" : "kept");
}

void model_test_exclusions_drop(void)
{
    s_slow_next = s_slow_count;
}

/* ---------- the test ---------- */

/* "1,2 secondes" (French) / "1.2 seconds"; "1,2 s" with short */
static void spoken_seconds(char *buf, size_t size, int ms, bool short_unit)
{
    const app_lang_t *lang = app_lang_get();
    int tenths = (ms + 50) / 100;
    snprintf(buf, size, "%d%c%d %s", tenths / 10, lang->decimal_mark, tenths % 10,
             short_unit ? "s" : (ms < lang->second_one_below_ms ? lang->second_one : lang->seconds));
}

#define FASTEST_COUNT       3

bool model_test_fastest(char *buf, size_t size)
{
    // The models offered now (a model removed from AGENTS.INI or excluded is not named), by mean time
    cJSON *times = load_times();
    int best[FASTEST_COUNT];
    double best_avg[FASTEST_COUNT];
    int count = 0;
    for (size_t i = 0; i < llm_model_count; i++) {
        const cJSON *e = cJSON_GetObjectItem(times, llm_models[i].id);
        if (number_of(e, "n") <= 0) {
            continue;
        }
        double avg = number_of(e, "avg");
        int at = count < FASTEST_COUNT ? count : FASTEST_COUNT;
        while (at > 0 && best_avg[at - 1] > avg) {
            at--;
        }
        if (at >= FASTEST_COUNT) {
            continue;
        }
        for (int k = (count < FASTEST_COUNT ? count : FASTEST_COUNT - 1); k > at; k--) {
            best[k] = best[k - 1];
            best_avg[k] = best_avg[k - 1];
        }
        best[at] = (int)i;
        best_avg[at] = avg;
        count += (count < FASTEST_COUNT);
    }
    cJSON_Delete(times);
    if (count == 0) {
        return false;
    }
    const app_lang_t *lang = app_lang_get();
    buf[0] = '\0';
    for (int k = 0; k < count; k++) {
        char time_text[32];
        spoken_seconds(time_text, sizeof(time_text), (int)best_avg[k], false);
        size_t n = strlen(buf);
        snprintf(buf + n, size - n, "%s%s (%s)", k == 0 ? "" : (k == count - 1 ? lang->and_word : ", "),
                 llm_models[best[k]].spoken, time_text);
    }
    return true;
}

/* The screen: a line per model tested, "01 Haiku : 0,8 s", the one being tested ending with "…" */
#define SCREEN_LINE_MAX     64

static void screen_line(char *screen, size_t size, size_t from, int number, const char *name, const char *result)
{
    snprintf(screen + from, size - from, "%s%02d %s%s%s", from ? "\n" : "", number, name,
             result ? " : " : "…", result ? result : "");
}

static void test_run(void)
{
    const app_lang_t *lang = app_lang_get();
    cJSON *times = load_times();
    size_t count = llm_model_count;
    const llm_model_t *best = NULL;
    int best_ms = 0;
    char text[200], time_text[32];
    size_t screen_size = (count + 1 + SPEECH_RESULTS_MAX) * SCREEN_LINE_MAX;
    char *screen = calloc(1, screen_size);
    // The speech models first: the speech is free (the results of the chat models are then spoken while testing)
    if (screen) {
        test_speech(screen, screen_size);
        strlcat(screen, "\n", screen_size);
    }
    ESP_LOGI(TAG, "Testing %u models", (unsigned)count);
    assistant_announce_begin();

    for (size_t i = 0; i < count && !s_cancel && screen; i++) {
        const llm_model_t *m = &llm_models[i];
        size_t line_start = strlen(screen);
        screen_line(screen, screen_size, line_start, (int)i + 1, m->spoken, NULL);
        assistant_show(screen);
        llm_stats_t st;
        llm_error_t err = llm_probe(m, lang->test_question, &st);
        ESP_LOGI(TAG, "%s: %s, first %d ms, total %d ms, %d tokens (reasoning %d)", m->id, status_of(err),
                 st.first_ms, st.total_ms, st.answer_tokens, st.reasoning_tokens);
        record(times, m, err, &st);
        save_times(times);
        write_speed_file(times, lang->test_question);
        if (s_cancel) {
            break;
        }
        bool answered = (err == LLM_OK && st.first_ms >= 0);
        if (answered) {
            if (!best || st.first_ms < best_ms) {
                best = m;
                best_ms = st.first_ms;
            }
            spoken_seconds(time_text, sizeof(time_text), st.first_ms, true);
            screen_line(screen, screen_size, line_start, (int)i + 1, m->spoken, time_text);
            spoken_seconds(time_text, sizeof(time_text), st.first_ms, false);
            snprintf(text, sizeof(text), lang->test_result, m->spoken, time_text);
        } else {
            screen_line(screen, screen_size, line_start, (int)i + 1, m->spoken, lang->test_unavailable_short);
            snprintf(text, sizeof(text), lang->test_unavailable, m->spoken);
        }
        assistant_show(screen);
        // Queued: spoken while the next model is tested
        assistant_announce(text);
    }

    if (!s_cancel) {
        classify(times);
        save_times(times);
        write_speed_file(times, lang->test_question);
        if (best) {
            snprintf(text, sizeof(text), lang->test_done, best->spoken);
        } else {
            strlcpy(text, lang->test_none, sizeof(text));
        }
        assistant_announce(text);
        assistant_announce_end();
        find_slow_free(times);
        if (!s_cancel && model_test_exclusion_question(text, sizeof(text))) {
            // Asked as an answer: the user's answer is listened to, then voice_cmd.c asks about the next one
            voice_cmd_expect_exclusion();
            assistant_say(text);
            cJSON_Delete(times);
            free(screen);
            ESP_LOGI(TAG, "Test done, slow free models asked about");
            return;
        }
        // The list stays on screen a while, then back to sleep (unless someone speaks meanwhile)
        for (int t = 0; t < MODEL_TEST_LIST_SHOWN_MS / 100 && !s_cancel; t++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!s_cancel) {
            assistant_test_done();
        }
    }
    cJSON_Delete(times);
    free(screen);
    ESP_LOGI(TAG, "Test %s", s_cancel ? "stopped" : "done");
}

void model_test_start(void)
{
    if (s_running) {
        return;
    }
    s_cancel = false;
    s_running = true;
    s_pending = true;
    s_slow_count = 0;
    s_slow_next = 0;
}

void model_test_poll(void)
{
    if (s_free_reload) {
        // Models excluded after the test: the free ones offered are chosen again from the last list read
        s_free_reload = false;
        openrouter_free_init();
        ui_ctrl_models_changed();
    }
    if (!s_pending) {
        return;
    }
    s_pending = false;
    test_run();
    s_running = false;
}
