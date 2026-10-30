/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Texts of each language, read from /spiffs/lang_<code>.txt (LANG_FR.TXT... on the configuration drive): one
 * "key = text" line per field of app_lang_t. The firmware's English file (main/lang/lang_en.txt, embedded) is the
 * base: a key missing in a file, or a text whose printf conversions (%s, %d...) differ from the English one, keeps
 * the English text. A language is read when first used and kept (PSRAM): its texts may still be in use by a task
 * when the language changes.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "file_lang.h"

static const char *TAG = "app_lang";

#define LANG_FILE_FORMAT    "/spiffs/lang_%s.txt"
#define LANG_FILE_MAX       (32 * 1024)     /* as on the configuration drive */

/* Files given with the firmware (main/lang/, embedded NUL-terminated), installed when missing or emptied */
extern const char s_default_fr_start[] asm("_binary_lang_fr_txt_start");
extern const char s_default_fr_end[] asm("_binary_lang_fr_txt_end");
extern const char s_default_en_start[] asm("_binary_lang_en_txt_start");
extern const char s_default_en_end[] asm("_binary_lang_en_txt_end");
extern const char s_default_nl_start[] asm("_binary_lang_nl_txt_start");
extern const char s_default_nl_end[] asm("_binary_lang_nl_txt_end");
extern const char s_default_de_start[] asm("_binary_lang_de_txt_start");
extern const char s_default_de_end[] asm("_binary_lang_de_txt_end");
extern const char s_default_es_start[] asm("_binary_lang_es_txt_start");
extern const char s_default_es_end[] asm("_binary_lang_es_txt_end");
extern const char s_default_it_start[] asm("_binary_lang_it_txt_start");
extern const char s_default_it_end[] asm("_binary_lang_it_txt_end");

static const struct {
    const char *code;
    const char *start;
    const char *end;
} s_defaults[] = {
    { "fr", s_default_fr_start, s_default_fr_end },
    { "en", s_default_en_start, s_default_en_end },
    { "nl", s_default_nl_start, s_default_nl_end },
    { "de", s_default_de_start, s_default_de_end },
    { "es", s_default_es_start, s_default_es_end },
    { "it", s_default_it_start, s_default_it_end },
};

/* Languages offered (their phrase and persona files are read too), from Languages of CONFIG.INI */
static char s_codes[LANG_MAX][LANG_CODE_SIZE];
static char s_names[LANG_MAX][LANG_NAME_SIZE];
static size_t s_count = 0;

typedef enum {
    FIELD_TEXT,     /* const char * */
    FIELD_LIST,     /* const char *[count], "a, b, c" */
    FIELD_CHAR,     /* char */
    FIELD_INT,      /* int */
} field_kind_t;

typedef struct {
    const char *key;
    size_t offset;
    field_kind_t kind;
    int count;
} lang_field_t;

/* The fields of app_lang_t (code aside), as named in the files */
static const lang_field_t s_fields[] = {
    { "listening", offsetof(app_lang_t, listening), FIELD_TEXT, 1 },
    { "heard", offsetof(app_lang_t, heard), FIELD_TEXT, 1 },
    { "thinking", offsetof(app_lang_t, thinking), FIELD_TEXT, 1 },
    { "sorry", offsetof(app_lang_t, sorry), FIELD_TEXT, 1 },
    { "wifi_connecting", offsetof(app_lang_t, wifi_connecting), FIELD_TEXT, 1 },
    { "wifi_failed", offsetof(app_lang_t, wifi_failed), FIELD_TEXT, 1 },
    { "system_prompt", offsetof(app_lang_t, system_prompt), FIELD_TEXT, 1 },
    { "tts_instructions", offsetof(app_lang_t, tts_instructions), FIELD_TEXT, 1 },
    { "tts_emotion", offsetof(app_lang_t, tts_emotion), FIELD_TEXT, 1 },
    { "tts_pace", offsetof(app_lang_t, tts_pace), FIELD_TEXT, 1 },
    { "stt_error", offsetof(app_lang_t, stt_error), FIELD_TEXT, 1 },
    { "memory_header", offsetof(app_lang_t, memory_header), FIELD_TEXT, 1 },
    { "memory_saved", offsetof(app_lang_t, memory_saved), FIELD_TEXT, 1 },
    { "memory_full", offsetof(app_lang_t, memory_full), FIELD_TEXT, 1 },
    { "memory_forgotten", offsetof(app_lang_t, memory_forgotten), FIELD_TEXT, 1 },
    { "memory_not_found", offsetof(app_lang_t, memory_not_found), FIELD_TEXT, 1 },
    { "memory_cleared", offsetof(app_lang_t, memory_cleared), FIELD_TEXT, 1 },
    { "memory_empty", offsetof(app_lang_t, memory_empty), FIELD_TEXT, 1 },
    { "memory_list_intro", offsetof(app_lang_t, memory_list_intro), FIELD_TEXT, 1 },
    { "lang_ask", offsetof(app_lang_t, lang_ask), FIELD_TEXT, 1 },
    { "lang_already", offsetof(app_lang_t, lang_already), FIELD_TEXT, 1 },
    { "lang_unknown", offsetof(app_lang_t, lang_unknown), FIELD_TEXT, 1 },
    { "lang_switched", offsetof(app_lang_t, lang_switched), FIELD_TEXT, 1 },
    { "models_first", offsetof(app_lang_t, models_first), FIELD_TEXT, 1 },
    { "models_next", offsetof(app_lang_t, models_next), FIELD_TEXT, 1 },
    { "models_free", offsetof(app_lang_t, models_free), FIELD_TEXT, 1 },
    { "and_word", offsetof(app_lang_t, and_word), FIELD_TEXT, 1 },
    { "language_added", offsetof(app_lang_t, language_added), FIELD_TEXT, 1 },
    { "lang_untranslated", offsetof(app_lang_t, lang_untranslated), FIELD_TEXT, 1 },
    { "lang_bad_format", offsetof(app_lang_t, lang_bad_format), FIELD_TEXT, 1 },
    { "language_names", offsetof(app_lang_t, language_names), FIELD_TEXT, 1 },
    { "numbers", offsetof(app_lang_t, numbers), FIELD_TEXT, 1 },
    { "test_question", offsetof(app_lang_t, test_question), FIELD_TEXT, 1 },
    { "test_start", offsetof(app_lang_t, test_start), FIELD_TEXT, 1 },
    { "test_running", offsetof(app_lang_t, test_running), FIELD_TEXT, 1 },
    { "test_unavailable_short", offsetof(app_lang_t, test_unavailable_short), FIELD_TEXT, 1 },
    { "test_result", offsetof(app_lang_t, test_result), FIELD_TEXT, 1 },
    { "test_unavailable", offsetof(app_lang_t, test_unavailable), FIELD_TEXT, 1 },
    { "test_done", offsetof(app_lang_t, test_done), FIELD_TEXT, 1 },
    { "test_none", offsetof(app_lang_t, test_none), FIELD_TEXT, 1 },
    { "exclude_ask", offsetof(app_lang_t, exclude_ask), FIELD_TEXT, 1 },
    { "exclude_ask_unavailable", offsetof(app_lang_t, exclude_ask_unavailable), FIELD_TEXT, 1 },
    { "exclude_yes", offsetof(app_lang_t, exclude_yes), FIELD_TEXT, 1 },
    { "exclude_no", offsetof(app_lang_t, exclude_no), FIELD_TEXT, 1 },
    { "exclude_finished", offsetof(app_lang_t, exclude_finished), FIELD_TEXT, 1 },
    { "fastest_models", offsetof(app_lang_t, fastest_models), FIELD_TEXT, 1 },
    { "fastest_none", offsetof(app_lang_t, fastest_none), FIELD_TEXT, 1 },
    { "test_declined", offsetof(app_lang_t, test_declined), FIELD_TEXT, 1 },
    { "radio_playing", offsetof(app_lang_t, radio_playing), FIELD_TEXT, 1 },
    { "radio_not_found", offsetof(app_lang_t, radio_not_found), FIELD_TEXT, 1 },
    { "radio_unreachable", offsetof(app_lang_t, radio_unreachable), FIELD_TEXT, 1 },
    { "radio_ask", offsetof(app_lang_t, radio_ask), FIELD_TEXT, 1 },
    { "radio_stopped", offsetof(app_lang_t, radio_stopped), FIELD_TEXT, 1 },
    { "radio_last", offsetof(app_lang_t, radio_last), FIELD_TEXT, 1 },
    { "radio_first", offsetof(app_lang_t, radio_first), FIELD_TEXT, 1 },
    { "radio_none", offsetof(app_lang_t, radio_none), FIELD_TEXT, 1 },
    { "radio_what", offsetof(app_lang_t, radio_what), FIELD_TEXT, 1 },
    { "radio_what_title", offsetof(app_lang_t, radio_what_title), FIELD_TEXT, 1 },
    { "radio_genres", offsetof(app_lang_t, radio_genres), FIELD_TEXT, 1 },
    { "radio_fav_added", offsetof(app_lang_t, radio_fav_added), FIELD_TEXT, 1 },
    { "radio_fav_already", offsetof(app_lang_t, radio_fav_already), FIELD_TEXT, 1 },
    { "radio_fav_removed", offsetof(app_lang_t, radio_fav_removed), FIELD_TEXT, 1 },
    { "radio_fav_not", offsetof(app_lang_t, radio_fav_not), FIELD_TEXT, 1 },
    { "radio_fav_none", offsetof(app_lang_t, radio_fav_none), FIELD_TEXT, 1 },
    { "radio_fav_full", offsetof(app_lang_t, radio_fav_full), FIELD_TEXT, 1 },
    { "radio_title", offsetof(app_lang_t, radio_title), FIELD_TEXT, 1 },
    { "radio_volume_label", offsetof(app_lang_t, radio_volume_label), FIELD_TEXT, 1 },
    { "radio_treble_label", offsetof(app_lang_t, radio_treble_label), FIELD_TEXT, 1 },
    { "radio_nothing", offsetof(app_lang_t, radio_nothing), FIELD_TEXT, 1 },
    { "radio_fav_empty", offsetof(app_lang_t, radio_fav_empty), FIELD_TEXT, 1 },
    { "decimal_mark", offsetof(app_lang_t, decimal_mark), FIELD_CHAR, 1 },
    { "second_one", offsetof(app_lang_t, second_one), FIELD_TEXT, 1 },
    { "seconds", offsetof(app_lang_t, seconds), FIELD_TEXT, 1 },
    { "second_one_below_ms", offsetof(app_lang_t, second_one_below_ms), FIELD_INT, 1 },
    { "provider_ask", offsetof(app_lang_t, provider_ask), FIELD_TEXT, 1 },
    { "provider_unknown", offsetof(app_lang_t, provider_unknown), FIELD_TEXT, 1 },
    { "model_ask_intro", offsetof(app_lang_t, model_ask_intro), FIELD_TEXT, 1 },
    { "model_current", offsetof(app_lang_t, model_current), FIELD_TEXT, 1 },
    { "model_already", offsetof(app_lang_t, model_already), FIELD_TEXT, 1 },
    { "model_unknown", offsetof(app_lang_t, model_unknown), FIELD_TEXT, 1 },
    { "model_switched", offsetof(app_lang_t, model_switched), FIELD_TEXT, 1 },
    { "voice_ask_intro", offsetof(app_lang_t, voice_ask_intro), FIELD_TEXT, 1 },
    { "voice_current", offsetof(app_lang_t, voice_current), FIELD_TEXT, 1 },
    { "voice_ask_question", offsetof(app_lang_t, voice_ask_question), FIELD_TEXT, 1 },
    { "voice_already", offsetof(app_lang_t, voice_already), FIELD_TEXT, 1 },
    { "voice_unknown", offsetof(app_lang_t, voice_unknown), FIELD_TEXT, 1 },
    { "country_ask", offsetof(app_lang_t, country_ask), FIELD_TEXT, 1 },
    { "city_ask", offsetof(app_lang_t, city_ask), FIELD_TEXT, 1 },
    { "country_confirm", offsetof(app_lang_t, country_confirm), FIELD_TEXT, 1 },
    { "city_confirm", offsetof(app_lang_t, city_confirm), FIELD_TEXT, 1 },
    { "country_unknown", offsetof(app_lang_t, country_unknown), FIELD_TEXT, 1 },
    { "place_saved", offsetof(app_lang_t, place_saved), FIELD_TEXT, 1 },
    { "place_cancelled", offsetof(app_lang_t, place_cancelled), FIELD_TEXT, 1 },
    { "location_line", offsetof(app_lang_t, location_line), FIELD_TEXT, 1 },
    { "config_need_pc", offsetof(app_lang_t, config_need_pc), FIELD_TEXT, 1 },
    { "config_opening", offsetof(app_lang_t, config_opening), FIELD_TEXT, 1 },
    { "restarting", offsetof(app_lang_t, restarting), FIELD_TEXT, 1 },
    { "clock_shown", offsetof(app_lang_t, clock_shown), FIELD_TEXT, 1 },
    { "costs_title", offsetof(app_lang_t, costs_title), FIELD_TEXT, 1 },
    { "costs_conversation", offsetof(app_lang_t, costs_conversation), FIELD_TEXT, 1 },
    { "costs_today", offsetof(app_lang_t, costs_today), FIELD_TEXT, 1 },
    { "costs_month", offsetof(app_lang_t, costs_month), FIELD_TEXT, 1 },
    { "costs_total", offsetof(app_lang_t, costs_total), FIELD_TEXT, 1 },
    { "costs_cost", offsetof(app_lang_t, costs_cost), FIELD_TEXT, 1 },
    { "clock_hidden", offsetof(app_lang_t, clock_hidden), FIELD_TEXT, 1 },
    { "new_conversation", offsetof(app_lang_t, new_conversation), FIELD_TEXT, 1 },
    { "voice_choppy", offsetof(app_lang_t, voice_choppy), FIELD_TEXT, 1 },
    { "speech_test_phrase", offsetof(app_lang_t, speech_test_phrase), FIELD_TEXT, 1 },
    { "test_tts_name", offsetof(app_lang_t, test_tts_name), FIELD_TEXT, 1 },
    { "test_stt_name", offsetof(app_lang_t, test_stt_name), FIELD_TEXT, 1 },
    { "stt_ask", offsetof(app_lang_t, stt_ask), FIELD_TEXT, 1 },
    { "tts_ask", offsetof(app_lang_t, tts_ask), FIELD_TEXT, 1 },
    { "speech_unknown", offsetof(app_lang_t, speech_unknown), FIELD_TEXT, 1 },
    { "speech_current", offsetof(app_lang_t, speech_current), FIELD_TEXT, 1 },
    { "speech_already", offsetof(app_lang_t, speech_already), FIELD_TEXT, 1 },
    { "speech_no_key", offsetof(app_lang_t, speech_no_key), FIELD_TEXT, 1 },
    { "stt_switched", offsetof(app_lang_t, stt_switched), FIELD_TEXT, 1 },
    { "tts_switched", offsetof(app_lang_t, tts_switched), FIELD_TEXT, 1 },
    { "speech_no_credit", offsetof(app_lang_t, speech_no_credit), FIELD_TEXT, 1 },
    { "home_line", offsetof(app_lang_t, home_line), FIELD_TEXT, 1 },
    { "weekdays", offsetof(app_lang_t, weekdays), FIELD_LIST, 7 },
    { "now_format", offsetof(app_lang_t, now_format), FIELD_TEXT, 1 },
    { "weekdays_long", offsetof(app_lang_t, weekdays_long), FIELD_LIST, 7 },
    { "months", offsetof(app_lang_t, months), FIELD_LIST, 12 },
    { "error_effort", offsetof(app_lang_t, error_effort), FIELD_TEXT, 1 },
    { "error.claude.no_key", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_CLAUDE][LLM_ERR_NO_KEY]), FIELD_TEXT, 1 },
    { "error.claude.network", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_CLAUDE][LLM_ERR_NETWORK]), FIELD_TEXT, 1 },
    { "error.claude.auth", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_CLAUDE][LLM_ERR_AUTH]), FIELD_TEXT, 1 },
    { "error.claude.credit", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_CLAUDE][LLM_ERR_CREDIT]), FIELD_TEXT, 1 },
    { "error.claude.busy", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_CLAUDE][LLM_ERR_BUSY]), FIELD_TEXT, 1 },
    { "error.claude.refusal", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_CLAUDE][LLM_ERR_REFUSAL]), FIELD_TEXT, 1 },
    { "error.claude.other", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_CLAUDE][LLM_ERR_OTHER]), FIELD_TEXT, 1 },
    { "error.openai.no_key", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_OPENAI][LLM_ERR_NO_KEY]), FIELD_TEXT, 1 },
    { "error.openai.network", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_OPENAI][LLM_ERR_NETWORK]), FIELD_TEXT, 1 },
    { "error.openai.auth", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_OPENAI][LLM_ERR_AUTH]), FIELD_TEXT, 1 },
    { "error.openai.credit", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_OPENAI][LLM_ERR_CREDIT]), FIELD_TEXT, 1 },
    { "error.openai.busy", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_OPENAI][LLM_ERR_BUSY]), FIELD_TEXT, 1 },
    { "error.openai.refusal", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_OPENAI][LLM_ERR_REFUSAL]), FIELD_TEXT, 1 },
    { "error.openai.other", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_OPENAI][LLM_ERR_OTHER]), FIELD_TEXT, 1 },
    { "error.gemini.no_key", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_GEMINI][LLM_ERR_NO_KEY]), FIELD_TEXT, 1 },
    { "error.gemini.network", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_GEMINI][LLM_ERR_NETWORK]), FIELD_TEXT, 1 },
    { "error.gemini.auth", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_GEMINI][LLM_ERR_AUTH]), FIELD_TEXT, 1 },
    { "error.gemini.credit", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_GEMINI][LLM_ERR_CREDIT]), FIELD_TEXT, 1 },
    { "error.gemini.busy", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_GEMINI][LLM_ERR_BUSY]), FIELD_TEXT, 1 },
    { "error.gemini.refusal", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_GEMINI][LLM_ERR_REFUSAL]), FIELD_TEXT, 1 },
    { "error.gemini.other", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_GEMINI][LLM_ERR_OTHER]), FIELD_TEXT, 1 },
    { "error.openrouter.no_key", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_OPENROUTER][LLM_ERR_NO_KEY]), FIELD_TEXT, 1 },
    { "error.openrouter.network", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_OPENROUTER][LLM_ERR_NETWORK]), FIELD_TEXT, 1 },
    { "error.openrouter.auth", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_OPENROUTER][LLM_ERR_AUTH]), FIELD_TEXT, 1 },
    { "error.openrouter.credit", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_OPENROUTER][LLM_ERR_CREDIT]), FIELD_TEXT, 1 },
    { "error.openrouter.busy", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_OPENROUTER][LLM_ERR_BUSY]), FIELD_TEXT, 1 },
    { "error.openrouter.refusal", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_OPENROUTER][LLM_ERR_REFUSAL]), FIELD_TEXT, 1 },
    { "error.openrouter.other", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_OPENROUTER][LLM_ERR_OTHER]), FIELD_TEXT, 1 },
    { "error.xai.no_key", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_XAI][LLM_ERR_NO_KEY]), FIELD_TEXT, 1 },
    { "error.xai.network", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_XAI][LLM_ERR_NETWORK]), FIELD_TEXT, 1 },
    { "error.xai.auth", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_XAI][LLM_ERR_AUTH]), FIELD_TEXT, 1 },
    { "error.xai.credit", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_XAI][LLM_ERR_CREDIT]), FIELD_TEXT, 1 },
    { "error.xai.busy", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_XAI][LLM_ERR_BUSY]), FIELD_TEXT, 1 },
    { "error.xai.refusal", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_XAI][LLM_ERR_REFUSAL]), FIELD_TEXT, 1 },
    { "error.xai.other", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_XAI][LLM_ERR_OTHER]), FIELD_TEXT, 1 },
    { "error.mistral.no_key", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_MISTRAL][LLM_ERR_NO_KEY]), FIELD_TEXT, 1 },
    { "error.mistral.network", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_MISTRAL][LLM_ERR_NETWORK]), FIELD_TEXT, 1 },
    { "error.mistral.auth", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_MISTRAL][LLM_ERR_AUTH]), FIELD_TEXT, 1 },
    { "error.mistral.credit", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_MISTRAL][LLM_ERR_CREDIT]), FIELD_TEXT, 1 },
    { "error.mistral.busy", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_MISTRAL][LLM_ERR_BUSY]), FIELD_TEXT, 1 },
    { "error.mistral.refusal", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_MISTRAL][LLM_ERR_REFUSAL]), FIELD_TEXT, 1 },
    { "error.mistral.other", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_MISTRAL][LLM_ERR_OTHER]), FIELD_TEXT, 1 },
    { "error.perplexity.no_key", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_PERPLEXITY][LLM_ERR_NO_KEY]), FIELD_TEXT, 1 },
    { "error.perplexity.network", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_PERPLEXITY][LLM_ERR_NETWORK]), FIELD_TEXT, 1 },
    { "error.perplexity.auth", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_PERPLEXITY][LLM_ERR_AUTH]), FIELD_TEXT, 1 },
    { "error.perplexity.credit", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_PERPLEXITY][LLM_ERR_CREDIT]), FIELD_TEXT, 1 },
    { "error.perplexity.busy", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_PERPLEXITY][LLM_ERR_BUSY]), FIELD_TEXT, 1 },
    { "error.perplexity.refusal", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_PERPLEXITY][LLM_ERR_REFUSAL]), FIELD_TEXT, 1 },
    { "error.perplexity.other", offsetof(app_lang_t, llm_errors[LLM_PROVIDER_PERPLEXITY][LLM_ERR_OTHER]), FIELD_TEXT, 1 },
    { "steps_title", offsetof(app_lang_t, steps_title), FIELD_TEXT, 1 },
    { "steps_content", offsetof(app_lang_t, steps_content), FIELD_TEXT, 1 },
    { "settings_title", offsetof(app_lang_t, settings_title), FIELD_TEXT, 1 },
    { "settings_language", offsetof(app_lang_t, settings_language), FIELD_TEXT, 1 },
    { "settings_model", offsetof(app_lang_t, settings_model), FIELD_TEXT, 1 },
    { "settings_voice", offsetof(app_lang_t, settings_voice), FIELD_TEXT, 1 },
    { "settings_tts", offsetof(app_lang_t, settings_tts), FIELD_TEXT, 1 },
    { "settings_stt", offsetof(app_lang_t, settings_stt), FIELD_TEXT, 1 },
    { "voice_sample", offsetof(app_lang_t, voice_sample), FIELD_TEXT, 1 },
    { "settings_save", offsetof(app_lang_t, settings_save), FIELD_TEXT, 1 },
    { "settings_saved", offsetof(app_lang_t, settings_saved), FIELD_TEXT, 1 },
    { "soft_title", offsetof(app_lang_t, soft_title), FIELD_TEXT, 1 },
    { "soft_text", offsetof(app_lang_t, soft_text), FIELD_TEXT, 1 },
    { "soft_yes", offsetof(app_lang_t, soft_yes), FIELD_TEXT, 1 },
    { "factory_title", offsetof(app_lang_t, factory_title), FIELD_TEXT, 1 },
    { "factory_text", offsetof(app_lang_t, factory_text), FIELD_TEXT, 1 },
    { "factory_yes", offsetof(app_lang_t, factory_yes), FIELD_TEXT, 1 },
    { "restore_no", offsetof(app_lang_t, restore_no), FIELD_TEXT, 1 },
    { "restore_done", offsetof(app_lang_t, restore_done), FIELD_TEXT, 1 },
    { "reset_title", offsetof(app_lang_t, reset_title), FIELD_TEXT, 1 },
    { "reset_content", offsetof(app_lang_t, reset_content), FIELD_TEXT, 1 },
};
#define FIELD_COUNT (sizeof(s_fields) / sizeof(s_fields[0]))

/* A language read: its texts point into its buffer (PSRAM, never freed) */
typedef struct lang_entry {
    char code[LANG_CODE_SIZE];
    app_lang_t texts;
    int untranslated;               /* texts added in English to its file when read */
    char bad_keys[160];             /* texts whose conversions differ from the English ones: "a, b" */
    bool reported;                  /* the problems above were shown */
    struct lang_entry *next;
} lang_entry_t;

static lang_entry_t *s_loaded = NULL;
static app_lang_t *s_base = NULL;           /* the firmware's English texts */
static const app_lang_t *s_current = NULL;
static SemaphoreHandle_t s_lock = NULL;

/* ---------- parsing ---------- */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
    return s;
}

/* "\n" and "\\" (and \" in a quoted text) replaced in place; quotes around the text removed */
static char *unescape(char *s)
{
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        s[n - 1] = '\0';
        s++;
    }
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (*r == '\\' && r[1]) {
            r++;
            *w++ = (*r == 'n') ? '\n' : *r;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
    return s;
}

/* The conversions of a printf format, "sd" for "%s ... %02d" ("%%" left out) */
static void conversions(const char *format, char *out, size_t size)
{
    size_t n = 0;
    for (const char *p = format; p && *p && n + 1 < size; p++) {
        if (*p != '%') {
            continue;
        }
        p++;
        if (*p == '%') {
            continue;
        }
        while (*p && strchr("-+ #0123456789.*hlLzjt", *p)) {
            p++;
        }
        if (*p) {
            out[n++] = *p;
        }
    }
    out[n] = '\0';
}

static const lang_field_t *field_of(const char *key)
{
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        if (strcmp(key, s_fields[i].key) == 0) {
            return &s_fields[i];
        }
    }
    return NULL;
}

/* What parse() found wrong in a file: keys whose conversions differ, and the markers to add to their lines */
#define LANG_FIXES_MAX      16

typedef struct {
    char *bad;                      /* "a, b" */
    size_t bad_size;
    struct {
        const char *key;
        char suffix[32];            /* " [TO DO: %s]" */
    } fixes[LANG_FIXES_MAX];
    size_t fix_count;
} parse_report_t;

static void add_bad_key(parse_report_t *report, const char *key)
{
    if (report && strlen(report->bad) + strlen(key) + 3 < report->bad_size) {
        strcat(report->bad, report->bad[0] ? ", " : "");
        strcat(report->bad, key);
    }
}

/*
 * A text lacking the last conversions of the English one ("%s" forgotten): those added at its end, marked, so that
 * its use cannot crash ("Il fait %s [TO DO: %s]"); NULL when it differs otherwise. Kept (PSRAM).
 */
static const char *with_missing_conversions(const char *value, const char *mine, const char *theirs)
{
    size_t n = strlen(mine);
    if (strncmp(mine, theirs, n) != 0) {
        return NULL;    // other conversions, or more: the English text is used
    }
    size_t size = strlen(value) + sizeof(" [TO DO: ]") + 3 * strlen(theirs + n);
    char *fixed = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!fixed) {
        return NULL;
    }
    strlcpy(fixed, value, size);
    strlcat(fixed, " [TO DO: ", size);
    for (const char *c = theirs + n; *c; c++) {
        char conv[4] = { '%', *c, c[1] ? ' ' : '\0', '\0' };
        strlcat(fixed, conv, size);
    }
    strlcat(fixed, "]", size);
    return fixed;
}

/*
 * The "key = text" lines of a text (modified in place and kept: the texts point into it) over texts. With
 * base, a text whose conversions (%s, %d...) differ from the base's is listed in the report: lacking the last ones,
 * they are added at its end (and the marker to write into the file is noted); else the English text is kept.
 */
static void parse(char *text, app_lang_t *texts, const app_lang_t *base, const char *code, parse_report_t *report)
{
    for (char *line = text; line && *line;) {
        char *end = strchr(line, '\n');
        if (end) {
            *end = '\0';
        }
        char *next = end ? end + 1 : NULL;
        char *t = trim(line);
        char *eq = (t[0] && t[0] != '#') ? strchr(t, '=') : NULL;
        if (eq) {
            *eq = '\0';
            const lang_field_t *field = field_of(trim(t));
            char *value = trim(eq + 1);
            uint8_t *slot = (uint8_t *)texts + (field ? field->offset : 0);
            if (!field) {
                ESP_LOGW(TAG, "%s: unknown key '%s'", code, trim(t));
            } else if (field->kind == FIELD_TEXT) {
                value = unescape(value);
                if (base) {
                    char mine[16], theirs[16];
                    conversions(value, mine, sizeof(mine));
                    conversions(*(const char **)((const uint8_t *)base + field->offset), theirs, sizeof(theirs));
                    if (strcmp(mine, theirs) != 0) {
                        add_bad_key(report, field->key);
                        const char *fixed = with_missing_conversions(value, mine, theirs);
                        ESP_LOGW(TAG, "%s: '%s' has the conversions '%s' instead of '%s' (English text): %s", code,
                                 field->key, mine[0] ? mine : "-", theirs, fixed ? "the missing ones added at its end"
                                 : "the English text is used");
                        if (fixed) {
                            *(const char **)slot = fixed;
                            if (report && report->fix_count < LANG_FIXES_MAX) {
                                report->fixes[report->fix_count].key = field->key;
                                strlcpy(report->fixes[report->fix_count].suffix, fixed + strlen(value),
                                        sizeof(report->fixes[0].suffix));
                                report->fix_count++;
                            }
                        }
                        line = next;
                        continue;
                    }
                }
                *(const char **)slot = value;
            } else if (field->kind == FIELD_LIST) {
                // "a, b, c": each item in place
                const char **items = (const char **)slot;
                int i = 0;
                for (char *item = value; item && i < field->count; i++) {
                    char *comma = strchr(item, ',');
                    if (comma) {
                        *comma = '\0';
                    }
                    items[i] = unescape(trim(item));
                    item = comma ? comma + 1 : NULL;
                }
                if (i < field->count) {
                    ESP_LOGW(TAG, "%s: '%s' has %d items instead of %d", code, field->key, i, field->count);
                }
            } else if (field->kind == FIELD_CHAR) {
                *(char *)slot = value[0] ? value[0] : '.';
            } else {
                *(int *)slot = atoi(value);
            }
        }
        line = next;
    }
}

/* ---------- files ---------- */

static const char *default_of(const char *code, size_t *len)
{
    for (size_t i = 0; i < sizeof(s_defaults) / sizeof(s_defaults[0]); i++) {
        if (strcasecmp(code, s_defaults[i].code) == 0) {
            *len = (size_t)(s_defaults[i].end - s_defaults[i].start) - 1;     // without the added NUL
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
    char *text = heap_caps_malloc(LANG_FILE_MAX, MALLOC_CAP_SPIRAM);
    size_t n = text ? fread(text, 1, LANG_FILE_MAX - 1, f) : 0;
    fclose(f);
    if (text) {
        text[n] = '\0';
        if (n == LANG_FILE_MAX - 1) {
            ESP_LOGW(TAG, "%s: cut at %d bytes (the end is ignored)", path, LANG_FILE_MAX - 1);
        }
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

/* The firmware's file of the language, when it has one (missing or emptied file) */
static void install_default(const char *code, const char *path)
{
    size_t len = 0;
    const char *def = default_of(code, &len);
    if (!def) {
        return;
    }
    FILE *f = fopen(path, "w");
    bool ok = f && fwrite(def, 1, len, f) == len;
    if (f) {
        ok &= (fclose(f) == 0);
    }
    ESP_LOGI(TAG, "%s %s (%u bytes)", ok ? "Installed" : "Could not install", path, (unsigned)len);
}

/* The line "key = ..." of a text, NULL if none */
static const char *key_line(const char *text, const char *key)
{
    size_t len = strlen(key);
    for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        const char *k = p;
        while (*k == ' ' || *k == '\t') {
            k++;
        }
        const char *after = k + len;
        if (strncmp(k, key, len) == 0) {
            while (*after == ' ' || *after == '\t') {
                after++;
            }
            if (*after == '=') {
                return p;
            }
        }
    }
    return NULL;
}

/*
 * Keys missing in the file of SPIFFS (added by a newer firmware, or forgotten) are appended to it, with the
 * comment lines above them: from the firmware's file of the language, else from the English one (counted in
 * english: to be translated); what is there (possibly edited) is kept. Returns true when something was added.
 */
static bool add_missing_keys(const char *code, const char *path, const char *text, int *english)
{
    size_t len = 0;
    const char *own = default_of(code, &len);
    const char *en = default_of("en", &len);
    FILE *f = NULL;
    *english = 0;
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        if (key_line(text, s_fields[i].key)) {
            continue;
        }
        const char *def = own;
        const char *line = own ? key_line(own, s_fields[i].key) : NULL;
        if (!line) {
            def = en;
            line = key_line(en, s_fields[i].key);
        }
        if (!line) {
            continue;
        }
        // The comment lines just above it go with it
        const char *start = line;
        while (start > def + 1 && start[-1] == '\n' && start[-2] != '\n') {
            const char *prev = start - 1;
            while (prev > def && prev[-1] != '\n') {
                prev--;
            }
            if (*prev != '#') {
                break;
            }
            start = prev;
        }
        const char *end = strchr(line, '\n');
        size_t n = end ? (size_t)(end + 1 - start) : strlen(start);
        if (!f && !(f = fopen(path, "a"))) {
            return false;
        }
        fputs("\n", f);
        if (def == own || s_fields[i].kind != FIELD_TEXT) {
            fwrite(start, 1, n, f);
        } else {
            // In English, to translate: "key = [TODO: text]" (inside the quotes of a quoted text)
            fwrite(start, 1, (size_t)(line - start), f);
            const char *eq = strchr(line, '=');
            const char *value = eq + 1;
            while (*value == ' ' || *value == '\t') {
                value++;
            }
            const char *value_end = end ? end : line + strlen(line);
            while (value_end > value && (value_end[-1] == '\r' || value_end[-1] == ' ')) {
                value_end--;
            }
            bool quoted = (value_end - value >= 2 && *value == '"' && value_end[-1] == '"');
            fwrite(line, 1, (size_t)(value - line), f);
            fprintf(f, "%s[TODO: %.*s]%s\n", quoted ? "\"" : "", (int)(value_end - value - (quoted ? 2 : 0)),
                    value + (quoted ? 1 : 0), quoted ? "\"" : "");
        }
        if (def != own) {
            (*english)++;
        }
        ESP_LOGI(TAG, "%s: '%s' added%s", path, s_fields[i].key, def != own ? " in English (to translate)" : "");
    }
    if (f) {
        fclose(f);
    }
    return f != NULL;
}

/*
 * The markers of the report written into the file: " [TO DO: %s]" at the end of the lines of the texts lacking
 * conversions (inside the quotes of a quoted text). Written to a copy, then renamed.
 */
static void write_fixes(const char *path, const parse_report_t *report)
{
    char *text = read_file(path);
    char tmp[56];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = text ? fopen(tmp, "w") : NULL;
    if (!f) {
        free(text);
        return;
    }
    for (char *line = text; line && *line;) {
        char *end = strchr(line, '\n');
        if (end) {
            *end = '\0';
        }
        char *next = end ? end + 1 : NULL;
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
            line[--len] = '\0';
        }
        const char *suffix = NULL;
        for (size_t i = 0; i < report->fix_count && !suffix; i++) {
            suffix = (key_line(line, report->fixes[i].key) == line) ? report->fixes[i].suffix : NULL;
        }
        const char *eq = suffix ? strchr(line, '=') : NULL;
        const char *quote = eq ? strchr(eq, '"') : NULL;
        if (suffix && quote && quote != line + len - 1 && line[len - 1] == '"') {
            fprintf(f, "%.*s%s\"\n", (int)(len - 1), line, suffix);
        } else {
            fprintf(f, "%s%s\n", line, suffix ? suffix : "");
        }
        line = next;
    }
    free(text);
    bool ok = (fclose(f) == 0);
    remove(path);
    ok &= (rename(tmp, path) == 0);
    ESP_LOGW(TAG, "%s: %u marker(s) [TO DO: ...] %s", path, (unsigned)report->fix_count, ok ? "written" : "NOT written");
}

/* The English texts of the firmware, the base of every language */
static void base_init(void)
{
    if (s_base) {
        return;
    }
    size_t len = 0;
    const char *def = default_of("en", &len);
    char *text = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    s_base = heap_caps_calloc(1, sizeof(app_lang_t), MALLOC_CAP_SPIRAM);
    assert(text && s_base);
    memcpy(text, def, len);
    text[len] = '\0';
    parse(text, s_base, NULL, "en (firmware)", NULL);
    s_base->code = "en";
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        if (s_fields[i].kind == FIELD_TEXT && !*(const char **)((uint8_t *)s_base + s_fields[i].offset)) {
            ESP_LOGE(TAG, "The firmware's English file has no '%s'", s_fields[i].key);
            *(const char **)((uint8_t *)s_base + s_fields[i].offset) = "";
        }
    }
}

/* A language read from its file (installed when missing), kept; NULL when it has no file nor default */
static const app_lang_t *load(const char *code)
{
    for (lang_entry_t *e = s_loaded; e; e = e->next) {
        if (strcasecmp(e->code, code) == 0) {
            return &e->texts;
        }
    }
    int64_t t0 = esp_timer_get_time();
    char path[48];
    snprintf(path, sizeof(path), LANG_FILE_FORMAT, code);
    char *text = read_file(path);
    int english = 0;
    if (is_blank(text)) {
        free(text);
        install_default(code, path);
        text = read_file(path);
    } else if (strlen(text) < LANG_FILE_MAX - 1 && add_missing_keys(code, path, text, &english)) {
        // (a file cut by read_file would get the keys of its end again at every start-up)
        free(text);
        text = read_file(path);
    }
    if (!text) {
        return NULL;
    }
    lang_entry_t *e = heap_caps_calloc(1, sizeof(lang_entry_t), MALLOC_CAP_SPIRAM);
    if (!e) {
        free(text);
        return NULL;
    }
    strlcpy(e->code, code, sizeof(e->code));
    e->texts = *s_base;             // what the file does not give stays English
    parse_report_t *report = heap_caps_calloc(1, sizeof(parse_report_t), MALLOC_CAP_SPIRAM);
    if (report) {
        report->bad = e->bad_keys;
        report->bad_size = sizeof(e->bad_keys);
    }
    parse(text, &e->texts, s_base, code, report);
    if (report && report->fix_count) {
        write_fixes(path, report);
    }
    free(report);
    e->texts.code = e->code;
    e->untranslated = english;
    e->next = s_loaded;
    s_loaded = e;
    ESP_LOGI(TAG, "%s read and checked in %lld ms (%d added in English, conversions differing: %s)", path,
             (esp_timer_get_time() - t0) / 1000, english, e->bad_keys[0] ? e->bad_keys : "none");
    return &e->texts;
}

/* A new language's file: a comment, then the English one of the firmware */
static bool create_from_english(const char *path, const char *name, const char *code, const char *def, size_t len)
{
    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        return false;
    }
    f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Could not make %s", path);
        return false;
    }
    fprintf(f, "# %s (%s): new language, made from the English file. Translate the texts below (keep the keys and the\n"
            "# %%s, %%d...). Until then, they are the English ones.\n\n", name, code);
    fwrite(def, 1, len, f);
    fclose(f);
    ESP_LOGI(TAG, "%s made for %s", path, name);
    return true;
}

bool app_lang_create(const char *code, const char *name)
{
    size_t len = 0;
    if (default_of(code, &len)) {
        // Given with the firmware: installed now when missing, to be seen on the configuration drive
        char own[48];
        snprintf(own, sizeof(own), LANG_FILE_FORMAT, code);
        char *text = read_file(own);
        if (is_blank(text)) {
            install_default(code, own);
        }
        free(text);
        return false;
    }
    char path[48];
    snprintf(path, sizeof(path), LANG_FILE_FORMAT, code);
    const char *def = default_of("en", &len);
    return create_from_english(path, name, code, def, len);
}

/* ---------- interface ---------- */

void app_lang_set(const char *code)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    base_init();
    if (!s_current) {
        s_current = s_base;
    }
    if (code == NULL || code[0] == '\0') {
        ESP_LOGW(TAG, "No language given, keeping %s", s_current->code);
        xSemaphoreGive(s_lock);
        return;
    }
    const app_lang_t *texts = load(code);
    if (texts) {
        s_current = texts;
        ESP_LOGI(TAG, "Language: %s", s_current->code);
        xSemaphoreGive(s_lock);
        return;
    }

    /* No texts for this language: English texts, but transcribe and answer in the requested language */
    lang_entry_t *e = heap_caps_calloc(1, sizeof(lang_entry_t), MALLOC_CAP_SPIRAM);
    if (e) {
        strlcpy(e->code, code, sizeof(e->code));
        e->texts = *s_base;
        e->texts.code = e->code;
        e->texts.system_prompt = "You are a voice assistant. Always answer in the same language as the user, "
                                 "concisely (a few sentences at most) and without Markdown formatting, "
                                 "because your answer will be read aloud.";
        e->next = s_loaded;
        s_loaded = e;
        s_current = &e->texts;
    }
    ESP_LOGW(TAG, "No texts for language '%s', using English texts", code);
    xSemaphoreGive(s_lock);
}

void app_lang_init(const char *languages)
{
    // "FR:Français, EN:English": codes in lower case, names as written
    s_count = 0;
    char *copy = strdup(languages ? languages : "");
    for (char *item = copy; item && *item && s_count < LANG_MAX;) {
        char *comma = strchr(item, ',');
        if (comma) {
            *comma = '\0';
        }
        char *colon = strchr(item, ':');
        char *code = trim(item);
        if (colon) {
            *colon = '\0';
            code = trim(item);
        }
        size_t len = strlen(code);
        bool valid = (len == 2);   // ISO 639-1, as the transcription expects; file names fit 8.3 ("PERSO_DE")
        for (size_t i = 0; i < len && valid; i++) {
            valid = (code[i] >= 'a' && code[i] <= 'z') || (code[i] >= 'A' && code[i] <= 'Z');
        }
        if (valid) {
            for (size_t i = 0; i <= len; i++) {
                s_codes[s_count][i] = (char)tolower((unsigned char)code[i]);
            }
            strlcpy(s_names[s_count], colon ? trim(colon + 1) : code, LANG_NAME_SIZE);
            s_count++;
        } else if (len) {
            ESP_LOGW(TAG, "Languages: '%s' is not a 2-letter language code (ISO 639-1): left out", code);
        }
        item = comma ? comma + 1 : NULL;
    }
    free(copy);
    if (s_count == 0) {
        strlcpy(s_codes[0], "fr", LANG_CODE_SIZE);
        strlcpy(s_names[0], "Français", LANG_NAME_SIZE);
        strlcpy(s_codes[1], "en", LANG_CODE_SIZE);
        strlcpy(s_names[1], "English", LANG_NAME_SIZE);
        s_count = 2;
    }
    ESP_LOGI(TAG, "%u language(s) offered", (unsigned)s_count);
}

size_t app_lang_count(void)
{
    return s_count;
}

const char *app_lang_code(size_t index)
{
    return index < s_count ? s_codes[index] : "";
}

const char *app_lang_name(size_t index)
{
    return index < s_count ? s_names[index] : "";
}

const app_lang_t *app_lang_find(const char *code)
{
    bool offered = false;
    for (size_t i = 0; i < app_lang_count(); i++) {
        offered |= (strcasecmp(code, app_lang_code(i)) == 0);
    }
    if (!offered) {
        return NULL;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    base_init();
    const app_lang_t *texts = load(code);
    xSemaphoreGive(s_lock);
    return texts;
}

bool app_lang_problems(char *out, size_t size)
{
    const app_lang_t *lang = app_lang_get();
    lang_entry_t *e = NULL;
    for (lang_entry_t *l = s_loaded; l; l = l->next) {
        e = (&l->texts == lang) ? l : e;
    }
    if (!e || e->reported || (!e->untranslated && !e->bad_keys[0])) {
        return false;
    }
    e->reported = true;
    char upper[LANG_CODE_SIZE];
    for (size_t i = 0; i < sizeof(upper); i++) {
        upper[i] = toupper((unsigned char)e->code[i]);
    }
    const char *name = e->code;
    for (size_t i = 0; i < app_lang_count(); i++) {
        name = strcasecmp(app_lang_code(i), e->code) == 0 ? app_lang_name(i) : name;
    }
    out[0] = '\0';
    if (e->untranslated) {
        snprintf(out, size, lang->lang_untranslated, upper, name);
    }
    if (e->bad_keys[0]) {
        size_t n = strlen(out);
        snprintf(out + n, size - n, "%s", n ? "\n\n" : "");
        n = strlen(out);
        snprintf(out + n, size - n, lang->lang_bad_format, upper, e->bad_keys);
    }
    return true;
}

const app_lang_t *app_lang_get(void)
{
    if (!s_current) {
        app_lang_set(NULL);     // the English texts until a language is set
    }
    return s_current;
}
