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
#include <strings.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "app_lang.h"
#include "app_memory.h"
#include "llm.h"
#include "settings.h"
#include "voice_cmd.h"
#include "phrases.h"
#include "assistant.h"
#include "tts_stream.h"
#include "app_ui_ctrl.h"
#include "driver/usb_serial_jtag.h"
#include "model_test.h"

#define CMD_NORM_SIZE           160
#define CMD_REPLY_SIZE          512
/*
 * After a question ("Quelle langue ?", "Quel modèle ?"), the answer is expected within this delay, counted from
 * the end of the question (the list of models takes 10 to 15 s to read)
 */
#define CMD_ANSWER_TIMEOUT_US   (30 * 1000 * 1000LL)

static const char *TAG = "voice_cmd";

/*
 * Phrase lists: their patterns are in the phrase files (main/phrases/phrases_<lang>.txt, VOICE_<LANG>.TXT on the
 * configuration drive), in the section named after the variable (phrases.h). Only the current language's
 * phrases are recognised, except "change la langue", recognised in every language.
 */
/* Said alone, they send the assistant back to sleep without an answer */
static PHRASE_LIST(s_goodbyes);
static PHRASE_LIST_ALL_LANGUAGES(s_change_lang);
static PHRASE_LIST(s_lang_shortcut);
static PHRASE_LIST(s_list_models);
static PHRASE_LIST(s_test_models);
static PHRASE_LIST(s_change_model);
static PHRASE_LIST(s_model_shortcut);
static PHRASE_LIST(s_change_voice);
static PHRASE_LIST(s_voice_shortcut);
/* A new conversation, as after "au revoir" (normalized: lowercase, apostrophes as spaces) */
static PHRASE_LIST(s_new_conversation);
/* Configuration files (normalized: "éditer" and "editer", "la config" and "le config" as often transcribed) */
static PHRASE_LIST(s_edit_config);
static PHRASE_LIST(s_restart);
static PHRASE_LIST(s_open_settings);
static PHRASE_LIST(s_change_country);
static PHRASE_LIST(s_change_city);
/* Words before the name in an answer ("j'habite à Evere"), skipped; articles are kept ("Le Havre") */
static PHRASE_LIST(s_answer_prefix);
static PHRASE_LIST(s_yes);
static PHRASE_LIST(s_no);

/*
 * Countries known by voice: ISO 3166-1 alpha-2 code (the web search needs it), French and English names,
 * other names they may be called by
 */
static const struct {
    const char *code;
    const char *fr;
    const char *en;
    const char *other;
} s_countries[] = {
    { "BE", "Belgique", "Belgium", NULL },
    { "FR", "France", "France", NULL },
    { "LU", "Luxembourg", "Luxembourg", NULL },
    { "CH", "Suisse", "Switzerland", NULL },
    { "NL", "Pays-Bas", "Netherlands", "hollande" },
    { "DE", "Allemagne", "Germany", NULL },
    { "AT", "Autriche", "Austria", NULL },
    { "IT", "Italie", "Italy", NULL },
    { "ES", "Espagne", "Spain", NULL },
    { "PT", "Portugal", "Portugal", NULL },
    { "GB", "Royaume-Uni", "United Kingdom", "angleterre" },
    { "IE", "Irlande", "Ireland", NULL },
    { "DK", "Danemark", "Denmark", NULL },
    { "SE", "Suède", "Sweden", NULL },
    { "NO", "Norvège", "Norway", NULL },
    { "FI", "Finlande", "Finland", NULL },
    { "PL", "Pologne", "Poland", NULL },
    { "CZ", "Tchéquie", "Czech Republic", "république tchèque" },
    { "GR", "Grèce", "Greece", NULL },
    { "HU", "Hongrie", "Hungary", NULL },
    { "RO", "Roumanie", "Romania", NULL },
    { "MC", "Monaco", "Monaco", NULL },
    { "CA", "Canada", "Canada", "québec" },
    { "US", "États-Unis", "United States", "amérique" },
    { "MX", "Mexique", "Mexico", NULL },
    { "BR", "Brésil", "Brazil", NULL },
    { "AR", "Argentine", "Argentina", NULL },
    { "MA", "Maroc", "Morocco", NULL },
    { "DZ", "Algérie", "Algeria", NULL },
    { "TN", "Tunisie", "Tunisia", NULL },
    { "SN", "Sénégal", "Senegal", NULL },
    { "CI", "Côte d'Ivoire", "Ivory Coast", NULL },
    { "CM", "Cameroun", "Cameroon", NULL },
    { "CD", "République démocratique du Congo", "DR Congo", "congo" },
    { "MG", "Madagascar", "Madagascar", NULL },
    { "JP", "Japon", "Japan", NULL },
    { "CN", "Chine", "China", NULL },
    { "IN", "Inde", "India", NULL },
    { "AU", "Australie", "Australia", NULL },
    { "NZ", "Nouvelle-Zélande", "New Zealand", NULL },
};

/* Words allowed after the answer of a shortcut ("parle anglais s'il te plaît", "modèle 3 please") */
static PHRASE_LIST(s_courtesy);

/* Languages offered, with the names they may be called by (in both languages, transcription variants) */
static const struct {
    const char *code;
    const char *names[6];
} s_languages[] = {
    { "fr", { "français", "francais", "french", NULL } },
    { "en", { "anglais", "english", "inglish", "l anglais", "angle", NULL } },     /* "angle": heard for "anglais" */
};

/* Numbers 1..13 as said (digits, French and English words, ordinals); "un"/"une" are articles, see find_model() */
static const char *const s_numbers[][7] = {
    { "1", "premier", "première", "one", "first", NULL, NULL },
    { "2", "deux", "deuxième", "second", "seconde", "two", NULL },
    { "3", "trois", "troisième", "three", "third", NULL, NULL },
    { "4", "quatre", "quatrième", "four", "fourth", NULL, NULL },
    { "5", "cinq", "cinquième", "five", "fifth", NULL, NULL },
    { "6", "six", "sixième", "sixth", NULL, NULL, NULL },
    { "7", "sept", "septième", "seven", "seventh", NULL, NULL },
    { "8", "huit", "huitième", "eight", "eighth", NULL, NULL },
    { "9", "neuf", "neuvième", "nine", "ninth", NULL, NULL },
    { "10", "dix", "dixième", "ten", "tenth", NULL, NULL },
    { "11", "onze", "onzième", "eleven", "eleventh", NULL, NULL },
    { "12", "douze", "douzième", "twelve", "twelfth", NULL, NULL },
    { "13", "treize", "treizième", "thirteen", "thirteenth", NULL, NULL },
};

typedef enum {
    ASKED_NOTHING,
    ASKED_LANGUAGE,
    ASKED_PROVIDER,
    ASKED_MODEL,
    ASKED_VOICE,
    ASKED_COUNTRY,
    ASKED_CITY,
    CONFIRM_COUNTRY,        /* "Confirmes-tu ... ?": s_pending holds the country code */
    CONFIRM_CITY,           /* s_pending holds the town */
    CONFIRM_EXCLUDE,        /* model test: exclude the slow free model asked about? */
} asked_t;

static asked_t s_asked = ASKED_NOTHING;
static int64_t s_asked_us = 0;
static char s_pending[48];

/* Normalized text starts with one of the phrases (alone or followed by more words); length matched in *len */
static bool starts_with(const char *norm, const char *const *phrases, size_t *len)
{
    for (; *phrases; phrases++) {
        size_t n = strlen(*phrases);
        if (strncmp(norm, *phrases, n) == 0 && (norm[n] == '\0' || norm[n] == ' ')) {
            if (len) {
                *len = n;
            }
            return true;
        }
    }
    return false;
}

/* The word appears as a whole word in the normalized text */
static bool has_word(const char *norm, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = strstr(norm, word); p; p = strstr(p + 1, word)) {
        if ((p == norm || p[-1] == ' ') && (p[n] == '\0' || p[n] == ' ')) {
            return true;
        }
    }
    return false;
}

/* The text after a shortcut is only `word`, possibly followed by "s'il te plaît", "please"... */
static bool is_only(const char *rest, const char *word)
{
    size_t n = strlen(word);
    if (strncmp(rest, word, n) != 0) {
        return false;
    }
    return rest[n] == '\0' || (rest[n] == ' ' && starts_with(rest + n + 1, phrases_of(&s_courtesy), NULL));
}

static char *reply_dup(const char *text)
{
    return strdup(text ? text : "");
}

/* ---------- language ---------- */

static const char *find_language(const char *norm)
{
    for (size_t i = 0; i < sizeof(s_languages) / sizeof(s_languages[0]); i++) {
        for (const char *const *name = s_languages[i].names; *name; name++) {
            if (has_word(norm, *name)) {
                return s_languages[i].code;
            }
        }
    }
    return NULL;
}

static bool is_language_only(const char *rest)
{
    for (size_t i = 0; i < sizeof(s_languages) / sizeof(s_languages[0]); i++) {
        for (const char *const *name = s_languages[i].names; *name; name++) {
            if (is_only(rest, *name)) {
                return true;
            }
        }
    }
    return false;
}

static voice_cmd_t choose_language(const char *norm, char **reply)
{
    const app_lang_t *lang = app_lang_get();
    const char *code = find_language(norm);
    if (!code) {
        *reply = reply_dup(lang->lang_unknown);
        return VOICE_CMD_ANSWERED;
    }
    if (strcasecmp(code, lang->code) == 0) {
        *reply = reply_dup(lang->lang_already);
        return VOICE_CMD_ANSWERED;
    }
    if (settings_save_str("Language", code) != ESP_OK) {
        *reply = reply_dup(lang->lang_unknown);
        return VOICE_CMD_ANSWERED;
    }
    ESP_LOGI(TAG, "language %s saved", code);
    assistant_set_language(code);
    // Confirmed in the new language (the voice reconnects with its instructions first)
    *reply = reply_dup(app_lang_get()->lang_switched);
    return VOICE_CMD_ANSWERED;
}

/* ---------- model ---------- */

/* "un" / "une" mean 1 only when said alone: elsewhere they are articles ("je veux un modèle rapide") */
static PHRASE_LIST(s_one_alone);

/* Providers as they may be said (normalized), by llm_provider_t; spoken as their company name */
static const struct {
    const char *name;
    const char *heard[7];
} s_providers[LLM_PROVIDER_COUNT] = {
    [LLM_PROVIDER_CLAUDE] = { "Anthropic", { "anthropic", "anthropique", "anthropik", "claude", "clode", NULL } },
    [LLM_PROVIDER_OPENAI] = { "OpenAI", { "openai", "open ai", "open a i", "openaï", "gpt", "chatgpt", "chat gpt" } },
    [LLM_PROVIDER_GEMINI] = { "Google", { "google", "gougle", "gemini", "gémini", "jemini", NULL } },
    [LLM_PROVIDER_OPENROUTER] = { "OpenRouter", { "openrouter", "open router", "open routeur", "openrouteur",
                                                  "open rooter", NULL } },
};

/* Provider named in the text, LLM_PROVIDER_COUNT if none */
static llm_provider_t find_provider(const char *norm)
{
    for (int p = 0; p < LLM_PROVIDER_COUNT; p++) {
        for (size_t i = 0; i < 7 && s_providers[p].heard[i]; i++) {
            if (has_word(norm, s_providers[p].heard[i])) {
                return (llm_provider_t)p;
            }
        }
    }
    return LLM_PROVIDER_COUNT;
}

/* Provider whose models were listed last: the numbers said next count within its models */
static llm_provider_t s_listed_provider = LLM_PROVIDER_COUNT;

/* Models of a provider (all for LLM_PROVIDER_COUNT) */
static bool model_listed(size_t i, llm_provider_t provider)
{
    return provider == LLM_PROVIDER_COUNT || llm_provider_of(llm_models[i].id) == provider;
}

static int model_count(llm_provider_t provider)
{
    int count = 0;
    for (size_t i = 0; i < llm_model_count; i++) {
        count += model_listed(i, provider);
    }
    return count;
}

/* Short names of the models as the transcription may write them (normalized) */
static const struct {
    const char *model;
    const char *heard[4];
} s_model_heard[] = {
    { "claude-haiku-5-5", { "haïku", "aïku", "aiku", NULL } },
    { "claude-sonnet-5-5", { "sonnette", "sonate", "sonnets", NULL } },
    { "claude-opus-5-5", { "opusse", "obus", NULL } },
    { "gpt-6-luna", { "lune", "lunar", NULL } },
    { "gpt-6.1-sol", { "sole", "saule", "soul", NULL } },
    { "gpt-6-astra", { "astrat", "astro", NULL } },
    { "gemini-3.5-flash-lite", { "flash light", "flashlight", "flash lait", NULL } },
    { "gemini-3.8-flash", { "flache", "flasch", NULL } },
    { "gemini-4-argon", { "argonne", "argone", NULL } },
};

/* Model number (1-based, in llm_models) named in the text by a word of its name ("sonnet"); 0 if none */
static int find_model_name(const char *norm)
{
    // Transcription variants first ("flash light" must not be taken for Flash)
    for (size_t h = 0; h < sizeof(s_model_heard) / sizeof(s_model_heard[0]); h++) {
        for (size_t k = 0; k < 4 && s_model_heard[h].heard[k]; k++) {
            if (!has_word(norm, s_model_heard[h].heard[k])) {
                continue;
            }
            for (size_t i = 0; i < llm_model_count; i++) {
                if (strcmp(llm_models[i].id, s_model_heard[h].model) == 0) {
                    return (int)i + 1;
                }
            }
        }
    }
    // Names: the words of the label made of letters only, except the provider ("haiku", "sonnet", "sol"...).
    // The model with the most of its words said wins: "flash lite" is Flash-Lite, "flash" alone is Flash.
    int best = 0, best_hits = 0, best_words = 0;
    for (size_t i = 0; i < llm_model_count; i++) {
        char label[48];
        app_text_normalize(llm_models[i].label, label, sizeof(label));
        int hits = 0, words = 0;
        for (char *word = strtok(label, " "); word; word = strtok(NULL, " ")) {
            bool letters = true;
            for (const char *c = word; *c; c++) {
                letters &= !isdigit((unsigned char)*c);
            }
            if (!letters || strcmp(word, "claude") == 0 || strcmp(word, "gpt") == 0 || strcmp(word, "gemini") == 0) {
                continue;
            }
            words++;
            hits += has_word(norm, word);
        }
        if (hits > best_hits || (hits == best_hits && hits > 0 && words < best_words)) {
            best = (int)i + 1;
            best_hits = hits;
            best_words = words;
        }
    }
    return best;
}

/*
 * Model (1-based, in llm_models) said in the text: its name, else its number among the models of the
 * provider (all models for LLM_PROVIDER_COUNT); 0 if none
 */
static int find_model(const char *norm, llm_provider_t provider)
{
    int named = find_model_name(norm);
    if (named) {
        return named;
    }
    int number = 0;
    for (const char *const *one = phrases_of(&s_one_alone); *one && !number; one++) {
        if (strcmp(norm, *one) == 0) {
            number = 1;
        }
    }
    for (size_t i = 0; i < sizeof(s_numbers) / sizeof(s_numbers[0]) && !number; i++) {
        for (size_t j = 0; j < sizeof(s_numbers[0]) / sizeof(s_numbers[0][0]) && s_numbers[i][j]; j++) {
            if (has_word(norm, s_numbers[i][j])) {
                number = (int)i + 1;
                break;
            }
        }
    }
    for (size_t i = 0; i < llm_model_count && number; i++) {
        if (model_listed(i, provider) && --number == 0) {
            return (int)i + 1;
        }
    }
    return 0;
}

static bool is_model_only(const char *rest)
{
    if (strcmp(rest, "un") == 0 || strcmp(rest, "une") == 0) {
        return true;
    }
    for (size_t i = 0; i < sizeof(s_numbers) / sizeof(s_numbers[0]); i++) {
        for (size_t j = 0; j < sizeof(s_numbers[0]) / sizeof(s_numbers[0][0]) && s_numbers[i][j]; j++) {
            if (is_only(rest, s_numbers[i][j])) {
                return true;
            }
        }
    }
    return false;
}

/* "Voici les modèles proposés par Anthropic. 1, Haiku, celui que j'utilise. 2, Sonnet. ... Dites son numéro." */
static char *model_list(llm_provider_t provider)
{
    const app_lang_t *lang = app_lang_get();
    const char *current = settings_get_parameter()->chat_model;
    char *out = malloc(CMD_REPLY_SIZE);
    if (!out) {
        return NULL;
    }
    snprintf(out, CMD_REPLY_SIZE, lang->model_ask_intro, s_providers[provider].name);
    unsigned number = 0;
    for (size_t i = 0; i < llm_model_count; i++) {
        if (!model_listed(i, provider)) {
            continue;
        }
        size_t n = strlen(out);
        snprintf(out + n, CMD_REPLY_SIZE - n, " %u, %s%s.", ++number, llm_models[i].spoken,
                 strcmp(llm_models[i].id, current) == 0 ? lang->model_current : "");
    }
    strlcat(out, " ", CMD_REPLY_SIZE);
    strlcat(out, lang->model_ask_question, CMD_REPLY_SIZE);
    return out;
}

/* A free OpenRouter model, read from its live list (openrouter_free.c) */
static bool is_free_model(const llm_model_t *m)
{
    size_t len = strlen(m->id);
    return strcmp(m->id, "openrouter/free") == 0 || (len > 5 && strcmp(m->id + len - 5, ":free") == 0);
}

/* Names of the models of a provider (free ones or not), "Haiku, Sonnet et Opus", appended to out */
static int append_names(char *out, size_t size, llm_provider_t provider, bool free_ones)
{
    const app_lang_t *lang = app_lang_get();
    int total = 0;
    for (size_t i = 0; i < llm_model_count; i++) {
        total += (llm_provider_of(llm_models[i].id) == provider && is_free_model(&llm_models[i]) == free_ones);
    }
    int n = 0;
    for (size_t i = 0; i < llm_model_count; i++) {
        if (llm_provider_of(llm_models[i].id) != provider || is_free_model(&llm_models[i]) != free_ones) {
            continue;
        }
        n++;
        strlcat(out, n == 1 ? "" : (n == total ? lang->and_word : ", "), size);
        strlcat(out, llm_models[i].spoken, size);
    }
    return total;
}

/* "Chez Anthropic, je supporte : Haiku, Sonnet et Opus. Chez OpenAI : ..." (the models of AGENTS.INI, by provider) */
static char *models_by_provider(void)
{
    const app_lang_t *lang = app_lang_get();
    const size_t size = 1536;
    char *out = calloc(1, size);
    char *names = malloc(size);
    if (!out || !names) {
        free(names);
        return out;
    }
    bool first = true;
    for (int p = 0; p < LLM_PROVIDER_COUNT; p++) {
        names[0] = '\0';
        if (append_names(names, size, (llm_provider_t)p, false) == 0) {
            continue;
        }
        size_t n = strlen(out);
        snprintf(out + n, size - n, "%s", n ? " " : "");
        n = strlen(out);
        snprintf(out + n, size - n, first ? lang->models_first : lang->models_next, s_providers[p].name, names);
        first = false;
    }
    names[0] = '\0';
    if (append_names(names, size, LLM_PROVIDER_OPENROUTER, true) > 0) {
        size_t n = strlen(out);
        snprintf(out + n, size - n, " ");
        n = strlen(out);
        snprintf(out + n, size - n, lang->models_free, names);
    }
    free(names);
    return out;
}

/* The models of the provider, numbered, then "which one?" (answered within CMD_ANSWER_TIMEOUT_US) */
static voice_cmd_t ask_model(llm_provider_t provider, char **reply)
{
    s_listed_provider = provider;
    s_asked = ASKED_MODEL;
    s_asked_us = esp_timer_get_time();
    *reply = model_list(provider);
    return VOICE_CMD_ANSWERED;
}

/* "Anthropic, OpenAI ou Google ?" (a model may also be named directly in the answer) */
static voice_cmd_t ask_provider(char **reply)
{
    s_asked = ASKED_PROVIDER;
    s_asked_us = esp_timer_get_time();
    *reply = reply_dup(app_lang_get()->provider_ask);
    return VOICE_CMD_ANSWERED;
}

/* Numbers count within the models of the provider (all models for LLM_PROVIDER_COUNT) */
static voice_cmd_t choose_model(const char *norm, llm_provider_t provider, char **reply)
{
    const app_lang_t *lang = app_lang_get();
    int number = find_model(norm, provider);
    char *text = NULL;
    if (number == 0) {
        asprintf(&text, lang->model_unknown, model_count(provider));
        *reply = text ? text : reply_dup("");
        return VOICE_CMD_ANSWERED;
    }
    const llm_model_t *model = &llm_models[number - 1];
    if (strcmp(model->id, settings_get_parameter()->chat_model) == 0) {
        asprintf(&text, lang->model_already, model->label);
        *reply = text ? text : reply_dup("");
        return VOICE_CMD_ANSWERED;
    }
    if (settings_save_str("Chat_model", model->id) != ESP_OK) {
        asprintf(&text, lang->model_unknown, model_count(provider));
        *reply = text ? text : reply_dup("");
        return VOICE_CMD_ANSWERED;
    }
    ESP_LOGI(TAG, "model %s saved", model->id);
    assistant_set_model(model->id);
    asprintf(&text, lang->model_switched, model->label);
    *reply = text ? text : reply_dup("");
    return VOICE_CMD_ANSWERED;
}

/* ---------- voice ---------- */

/* Names of the voices as the transcription may write them (normalized), besides the name itself */
static const struct {
    const char *voice;
    const char *heard[4];
} s_voice_heard[] = {
    { "marin", { "marine", "marina", NULL } },
    { "cedar", { "cèdre", "cedre", "sidar", "cédar" } },
    { "alloy", { "aloy", "alloi", "aloï", NULL } },
    { "ash", { "ashe", "hache", NULL } },
    { "ballad", { "ballade", "balade", NULL } },
    { "coral", { "corail", "corale", NULL } },
    { "echo", { "écho", "eco", NULL } },
    { "shimmer", { "shimer", "chimmer", NULL } },
    { "verse", { "vers", "verce", NULL } },
};

/* Voice number (1-based) said in the text: its name, else a number; 0 if none */
static int find_voice(const char *norm)
{
    for (size_t i = 0; i < tts_voice_count; i++) {
        if (has_word(norm, tts_voices[i])) {
            return (int)i + 1;
        }
        for (size_t j = 0; j < sizeof(s_voice_heard) / sizeof(s_voice_heard[0]); j++) {
            if (strcmp(s_voice_heard[j].voice, tts_voices[i]) != 0) {
                continue;
            }
            for (size_t k = 0; k < 4 && s_voice_heard[j].heard[k]; k++) {
                if (has_word(norm, s_voice_heard[j].heard[k])) {
                    return (int)i + 1;
                }
            }
        }
    }
    for (const char *const *one = phrases_of(&s_one_alone); *one; one++) {
        if (strcmp(norm, *one) == 0) {
            return 1;
        }
    }
    for (size_t i = 0; i < sizeof(s_numbers) / sizeof(s_numbers[0]) && i < tts_voice_count; i++) {
        for (size_t j = 0; j < sizeof(s_numbers[0]) / sizeof(s_numbers[0][0]) && s_numbers[i][j]; j++) {
            if (has_word(norm, s_numbers[i][j])) {
                return (int)i + 1;
            }
        }
    }
    return 0;
}

/* After "prends la voix": only a voice name or a number ("la voix est trop forte" is not a command) */
static bool is_voice_only(const char *rest)
{
    if (is_model_only(rest)) {
        return true;    // a number
    }
    char word[24];
    strlcpy(word, rest, sizeof(word));
    char *space = strchr(word, ' ');
    if (space) {
        *space = '\0';
    }
    return word[0] && find_voice(word) && is_only(rest, word);
}

/* Voice name as spoken and shown: "Cedar" */
static void voice_display_name(const char *voice, char *out, size_t size)
{
    strlcpy(out, voice, size);
    out[0] = toupper((unsigned char)out[0]);
}

/* "Voici les voix disponibles. 1, Marin, celle que j'utilise. 2, Cedar. ... Dites son numéro ou son nom." */
static char *voice_list(void)
{
    const app_lang_t *lang = app_lang_get();
    const char *current = settings_get_parameter()->voice;
    char *out = malloc(CMD_REPLY_SIZE);
    if (!out) {
        return NULL;
    }
    strlcpy(out, lang->voice_ask_intro, CMD_REPLY_SIZE);
    for (size_t i = 0; i < tts_voice_count; i++) {
        char name[24];
        voice_display_name(tts_voices[i], name, sizeof(name));
        size_t n = strlen(out);
        snprintf(out + n, CMD_REPLY_SIZE - n, " %u, %s%s.", (unsigned)(i + 1), name,
                 strcmp(tts_voices[i], current) == 0 ? lang->voice_current : "");
    }
    strlcat(out, " ", CMD_REPLY_SIZE);
    strlcat(out, lang->voice_ask_question, CMD_REPLY_SIZE);
    return out;
}

/* Saved and applied at once: the confirmation is the sample of the new voice, spoken with it */
static voice_cmd_t choose_voice(const char *norm, char **reply)
{
    const app_lang_t *lang = app_lang_get();
    int number = find_voice(norm);
    char *text = NULL;
    if (number == 0) {
        asprintf(&text, lang->voice_unknown, (int)tts_voice_count);
        *reply = text ? text : reply_dup("");
        return VOICE_CMD_ANSWERED;
    }
    const char *voice = tts_voices[number - 1];
    char name[24];
    voice_display_name(voice, name, sizeof(name));
    sys_param_t *param = settings_get_parameter();
    if (strcmp(voice, param->voice) == 0) {
        asprintf(&text, lang->voice_already, name);
        *reply = text ? text : reply_dup("");
        return VOICE_CMD_ANSWERED;
    }
    if (settings_save_str("Voice", voice) != ESP_OK) {
        asprintf(&text, lang->voice_unknown, (int)tts_voice_count);
        *reply = text ? text : reply_dup("");
        return VOICE_CMD_ANSWERED;
    }
    ESP_LOGI(TAG, "voice %s saved", voice);
    strlcpy(param->voice, voice, sizeof(param->voice));
    tts_stream_set_voice(voice);
    ui_ctrl_settings_sync();
    asprintf(&text, lang->voice_sample, name);
    *reply = text ? text : reply_dup("");
    return VOICE_CMD_ANSWERED;
}

/* ---------- dispatch ---------- */

/* ---------- country and town ---------- */

/* Name of a country in the current language */
static const char *country_name(size_t i)
{
    return strcasecmp(app_lang_get()->code, "fr") == 0 ? s_countries[i].fr : s_countries[i].en;
}

static int find_country(const char *norm)
{
    for (size_t i = 0; i < sizeof(s_countries) / sizeof(s_countries[0]); i++) {
        const char *names[] = { s_countries[i].fr, s_countries[i].en, s_countries[i].other };
        for (size_t j = 0; j < 3; j++) {
            char name[48];
            if (!names[j]) {
                continue;
            }
            app_text_normalize(names[j], name, sizeof(name));
            if (has_word(norm, name)) {
                return (int)i;
            }
        }
    }
    return -1;
}

/* The answer without "j'habite à", "c'est" ... and without the final punctuation: "Evere" */
static void answer_text(const char *text, const char *norm, char *out, size_t size)
{
    size_t len = 0;
    int words = 0;
    if (starts_with(norm, phrases_of(&s_answer_prefix), &len)) {
        words = 1;
        for (size_t i = 0; i < len; i++) {
            words += (norm[i] == ' ');
        }
    }
    const char *p = text;
    for (int w = 0; w < words; w++) {
        while (*p && !(isalnum((unsigned char)*p) || ((unsigned char)*p & 0x80))) {
            p++;
        }
        while (*p && (isalnum((unsigned char)*p) || ((unsigned char)*p & 0x80))) {
            p++;
        }
    }
    while (*p && !(isalnum((unsigned char)*p) || ((unsigned char)*p & 0x80))) {
        p++;
    }
    strlcpy(out, p, size);
    size_t n = strlen(out);
    while (n > 0 && (isspace((unsigned char)out[n - 1]) || strchr(".!?,;", out[n - 1]))) {
        out[--n] = '\0';
    }
    if (out[0] && !((unsigned char)out[0] & 0x80)) {
        out[0] = toupper((unsigned char)out[0]);
    }
}

static voice_cmd_t ask_confirmation(asked_t confirm, const char *format, const char *value, char **reply)
{
    s_asked = confirm;
    s_asked_us = esp_timer_get_time();
    char *text = NULL;
    asprintf(&text, format, value);
    *reply = text ? text : reply_dup("");
    return VOICE_CMD_ANSWERED;
}

/* "Oui" saves the pending country or town, anything else cancels */
static voice_cmd_t confirm_place(asked_t confirm, const char *norm, char **reply)
{
    const app_lang_t *lang = app_lang_get();
    bool yes = false;
    for (const char *const *word = phrases_of(&s_yes); *word && !yes; word++) {
        yes = has_word(norm, *word);
    }
    if (yes && has_word(norm, "non")) {
        yes = false;    // "oui ... non": not a clear yes
    }
    if (!yes) {
        *reply = reply_dup(lang->place_cancelled);
        return VOICE_CMD_ANSWERED;
    }
    bool country = (confirm == CONFIRM_COUNTRY);
    if (settings_save_str(country ? "Country" : "City", s_pending) != ESP_OK) {
        *reply = reply_dup(lang->place_cancelled);
        return VOICE_CMD_ANSWERED;
    }
    ESP_LOGI(TAG, "%s saved: %s", country ? "country" : "city", s_pending);
    assistant_set_location(country ? s_pending : NULL, country ? NULL : s_pending);
    *reply = reply_dup(lang->place_saved);
    return VOICE_CMD_ANSWERED;
}

void voice_cmd_expect_exclusion(void)
{
    s_asked = CONFIRM_EXCLUDE;
    s_asked_us = esp_timer_get_time();
}

static bool has_any(const char *norm, const phrase_list_t *list)
{
    for (const char *const *word = phrases_of(list); *word; word++) {
        if (has_word(norm, *word)) {
            return true;
        }
    }
    return false;
}

/* "Oui": the model is excluded, "non": kept; then the next one is asked. Anything else: NONE (handled as usual) */
static voice_cmd_t answer_exclusion(const char *norm, char **reply)
{
    const app_lang_t *lang = app_lang_get();
    bool no = has_any(norm, &s_no);
    bool yes = !no && has_any(norm, &s_yes);
    if (!yes && !no) {
        model_test_exclusions_drop();
        return VOICE_CMD_NONE;
    }
    model_test_exclusion_answer(yes);
    char question[200];
    bool more = model_test_exclusion_question(question, sizeof(question));
    if (more) {
        voice_cmd_expect_exclusion();
    }
    char *text = NULL;
    asprintf(&text, "%s %s", yes ? lang->exclude_yes : lang->exclude_no, more ? question : lang->exclude_finished);
    *reply = text ? text : reply_dup("");
    return VOICE_CMD_ANSWERED;
}

void voice_cmd_reply_spoken(void)
{
    if (s_asked != ASKED_NOTHING) {
        s_asked_us = esp_timer_get_time();
    }
}

bool voice_cmd_is_goodbye(const char *text)
{
    char norm[CMD_NORM_SIZE];
    app_text_normalize(text, norm, sizeof(norm));
    for (const char *const *phrase = phrases_of(&s_goodbyes); *phrase; phrase++) {
        if (strcmp(norm, *phrase) == 0) {
            return true;
        }
    }
    return false;
}

bool voice_cmd_ends_with_goodbye(const char *sentence)
{
    if (voice_cmd_is_goodbye(sentence)) {
        return true;
    }
    // "Avec plaisir, bonne soirée !": the part after the last comma (or ; :)
    const char *last = NULL;
    for (const char *p = sentence; *p; p++) {
        if (*p == ',' || *p == ';' || *p == ':') {
            last = p + 1;
        }
    }
    return last && voice_cmd_is_goodbye(last);
}

void voice_cmd_init(void)
{
    phrase_list_t *lists[] = {
        &s_goodbyes, &s_list_models, &s_test_models, &s_change_lang, &s_lang_shortcut, &s_change_model, &s_model_shortcut, &s_change_voice, &s_voice_shortcut,
        &s_new_conversation, &s_edit_config, &s_restart, &s_open_settings, &s_change_country, &s_change_city,
        &s_answer_prefix, &s_yes, &s_no, &s_courtesy, &s_one_alone,
    };
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
        phrases_of(lists[i]);
    }
}

voice_cmd_t voice_cmd_handle(const char *text, char **reply)
{
    char norm[CMD_NORM_SIZE];
    app_text_normalize(text, norm, sizeof(norm));
    *reply = NULL;

    // Answer to the question asked just before
    asked_t asked = s_asked;
    bool recent = s_asked_us && esp_timer_get_time() - s_asked_us < CMD_ANSWER_TIMEOUT_US;
    s_asked = ASKED_NOTHING;
    s_asked_us = 0;
    if (recent && asked == ASKED_LANGUAGE) {
        return choose_language(norm, reply);
    }
    if (recent && asked == ASKED_PROVIDER) {
        // A model may be named at once ("Sonnet"); else the provider's models are listed
        if (find_model_name(norm)) {
            return choose_model(norm, LLM_PROVIDER_COUNT, reply);
        }
        llm_provider_t provider = find_provider(norm);
        if (provider == LLM_PROVIDER_COUNT) {
            s_asked = ASKED_PROVIDER;   // asked again
            s_asked_us = esp_timer_get_time();
            *reply = reply_dup(app_lang_get()->provider_unknown);
            return VOICE_CMD_ANSWERED;
        }
        return ask_model(provider, reply);
    }
    if (recent && asked == ASKED_MODEL) {
        return choose_model(norm, s_listed_provider, reply);
    }
    if (recent && asked == ASKED_VOICE) {
        return choose_voice(norm, reply);
    }
    if (recent && asked == ASKED_COUNTRY) {
        int i = find_country(norm);
        if (i < 0) {
            *reply = reply_dup(app_lang_get()->country_unknown);
            return VOICE_CMD_ANSWERED;
        }
        strlcpy(s_pending, s_countries[i].code, sizeof(s_pending));
        return ask_confirmation(CONFIRM_COUNTRY, app_lang_get()->country_confirm, country_name(i), reply);
    }
    if (recent && asked == ASKED_CITY) {
        answer_text(text, norm, s_pending, sizeof(s_pending));
        if (!s_pending[0]) {
            *reply = reply_dup(app_lang_get()->place_cancelled);
            return VOICE_CMD_ANSWERED;
        }
        return ask_confirmation(CONFIRM_CITY, app_lang_get()->city_confirm, s_pending, reply);
    }
    if (recent && (asked == CONFIRM_COUNTRY || asked == CONFIRM_CITY)) {
        return confirm_place(asked, norm, reply);
    }
    if (recent && asked == CONFIRM_EXCLUDE) {
        voice_cmd_t answered = answer_exclusion(norm, reply);
        if (answered != VOICE_CMD_NONE) {
            return answered;
        }
    }

    size_t len = 0;
    if (starts_with(norm, phrases_of(&s_new_conversation), &len)) {
        // Alone (or with "s'il te plaît"): answered here; followed by a question: the model answers it
        const char *rest = norm + len + (norm[len] == ' ');
        if (!*rest || starts_with(rest, phrases_of(&s_courtesy), NULL)) {
            *reply = reply_dup(app_lang_get()->new_conversation);
            return VOICE_CMD_NEW_CONVERSATION;
        }
        return VOICE_CMD_NEW_CONVERSATION_ASK;
    }
    if (starts_with(norm, phrases_of(&s_change_lang), NULL)) {
        // "Change la langue en anglais": the language may already be given
        if (find_language(norm)) {
            return choose_language(norm, reply);
        }
        s_asked = ASKED_LANGUAGE;
        s_asked_us = esp_timer_get_time();
        *reply = reply_dup(app_lang_get()->lang_ask);
        return VOICE_CMD_ANSWERED;
    }
    if (starts_with(norm, phrases_of(&s_test_models), NULL)) {
        if (model_test_running()) {
            *reply = reply_dup(app_lang_get()->test_running);
            return VOICE_CMD_ANSWERED;
        }
        char text[160];
        snprintf(text, sizeof(text), app_lang_get()->test_start, (int)llm_model_count);
        *reply = reply_dup(text);
        return VOICE_CMD_TEST_MODELS;
    }
    if (starts_with(norm, phrases_of(&s_list_models), NULL)) {
        *reply = models_by_provider();
        return VOICE_CMD_ANSWERED;
    }
    if (starts_with(norm, phrases_of(&s_change_model), &len)) {
        // "Change de modèle pour Sonnet" / "pour le 3": the model may already be given (numbers over all
        // models), "change de modèle pour Google": its models are listed; else the provider is asked first
        if (find_model(norm + len, LLM_PROVIDER_COUNT)) {
            return choose_model(norm + len, LLM_PROVIDER_COUNT, reply);
        }
        llm_provider_t provider = find_provider(norm + len);
        return provider == LLM_PROVIDER_COUNT ? ask_provider(reply) : ask_model(provider, reply);
    }
    if (starts_with(norm, phrases_of(&s_change_voice), &len)) {
        // "Change de voix pour Cedar": the voice may already be given
        if (find_voice(norm + len)) {
            return choose_voice(norm + len, reply);
        }
        s_asked = ASKED_VOICE;
        s_asked_us = esp_timer_get_time();
        *reply = voice_list();
        return VOICE_CMD_ANSWERED;
    }
    if (starts_with(norm, phrases_of(&s_restart), &len)) {
        // Alone (or with "s'il te plaît"): "redémarre la box" or "relance la musique" are not this command
        const char *rest = norm + len + (norm[len] == ' ');
        if (!*rest || starts_with(rest, phrases_of(&s_courtesy), NULL)) {
            *reply = reply_dup(app_lang_get()->restarting);
            return VOICE_CMD_RESTART;
        }
    }
    if (starts_with(norm, phrases_of(&s_edit_config), NULL)) {
        // The files appear as a USB drive on the PC: without a PC (SOF packets from a host), nothing to show them
        if (!usb_serial_jtag_is_connected()) {
            *reply = reply_dup(app_lang_get()->config_need_pc);
            return VOICE_CMD_ANSWERED;
        }
        *reply = reply_dup(app_lang_get()->config_opening);
        return VOICE_CMD_EDIT_CONFIG;
    }
    if (starts_with(norm, phrases_of(&s_open_settings), NULL)) {
        return VOICE_CMD_OPEN_SETTINGS;     // shown at once, nothing spoken
    }
    if (starts_with(norm, phrases_of(&s_change_country), NULL)) {
        s_asked = ASKED_COUNTRY;
        s_asked_us = esp_timer_get_time();
        *reply = reply_dup(app_lang_get()->country_ask);
        return VOICE_CMD_ANSWERED;
    }
    if (starts_with(norm, phrases_of(&s_change_city), NULL)) {
        s_asked = ASKED_CITY;
        s_asked_us = esp_timer_get_time();
        *reply = reply_dup(app_lang_get()->city_ask);
        return VOICE_CMD_ANSWERED;
    }
    // Shortcuts: only when the rest is just a language name or a model number ("parle plus fort" is not one)
    if (starts_with(norm, phrases_of(&s_lang_shortcut), &len) && is_language_only(norm[len] ? norm + len + 1 : norm + len)) {
        return choose_language(norm + len, reply);
    }
    if (starts_with(norm, phrases_of(&s_model_shortcut), &len) && is_model_only(norm[len] ? norm + len + 1 : norm + len)) {
        return choose_model(norm + len, LLM_PROVIDER_COUNT, reply);
    }
    if (starts_with(norm, phrases_of(&s_voice_shortcut), &len) && is_voice_only(norm[len] ? norm + len + 1 : norm + len)) {
        return choose_voice(norm + len, reply);
    }
    return VOICE_CMD_NONE;
}
