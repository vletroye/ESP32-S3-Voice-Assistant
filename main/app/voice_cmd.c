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
#include "assistant.h"

#define CMD_NORM_SIZE           160
#define CMD_REPLY_SIZE          512
/*
 * After a question ("Quelle langue ?", "Quel modèle ?"), the answer is expected within this delay, counted from
 * the end of the question (the list of models takes 10 to 15 s to read)
 */
#define CMD_ANSWER_TIMEOUT_US   (30 * 1000 * 1000LL)

static const char *TAG = "voice_cmd";

/* Phrases as normalized by app_text_normalize(); French and English are both accepted */
static const char *const s_change_lang[] = {
    "change la langue", "changer la langue", "change de langue", "changer de langue", "change ta langue",
    "change the language", "change language", "switch language", "switch the language", NULL
};
static const char *const s_lang_shortcut[] = {
    "parle en", "parle", "parlons en", "parlons", "on parle", "passe en", "passe à l", "réponds en", "speak in",
    "speak", "switch to", "answer in", NULL
};
static const char *const s_change_model[] = {
    "change de modèle", "change le modèle", "changer de modèle", "changer le modèle", "change de modele",
    "change le modele", "change ton modèle", "change the model", "change model", "switch model",
    "switch the model", NULL
};
static const char *const s_model_shortcut[] = {
    "utilise le modèle", "utilise le modele", "prends le modèle", "passe au modèle", "passe au modele",
    "modèle numéro", "modèle", "modele", "use model", "use the model", "switch to model", "model number",
    "model", NULL
};
/* A new conversation, as after "au revoir" (normalized: lowercase, apostrophes as spaces) */
static const char *const s_new_conversation[] = {
    "démarrons une nouvelle discussion", "démarrons une nouvelle conversation", "commençons une nouvelle discussion",
    "commençons une nouvelle conversation", "démarre une nouvelle discussion", "démarre une nouvelle conversation",
    "commence une nouvelle discussion", "commence une nouvelle conversation", "on démarre une nouvelle discussion",
    "on commence une nouvelle discussion", "on recommence à zéro", "recommençons à zéro", "nouvelle discussion",
    "nouvelle conversation", "j ai une autre question", "une autre question", "autre question",
    "passons à autre chose", "on passe à autre chose", "parlons d autre chose", "changeons de sujet",
    "on change de sujet", "changement de sujet", "let s start a new conversation", "start a new conversation",
    "new conversation", "new chat", "i have another question", "another question", "let s move on",
    "let s talk about something else", "let s change the subject", "change of subject", NULL
};
static const char *const s_open_settings[] = {
    "ouvre les paramètres", "montre les paramètres", "affiche les paramètres", "ouvre les réglages",
    "montre les réglages", "affiche les réglages", "va dans les paramètres",
    "va dans les réglages", "open the settings", "show the settings", "open settings", "show settings",
    "display the settings", "go to the settings", NULL
};
static const char *const s_change_country[] = {
    "change mon pays", "changer mon pays", "change de pays", "changer de pays", "change le pays",
    "change my country", "change the country", NULL
};
static const char *const s_change_city[] = {
    "change ma ville", "changer ma ville", "change de ville", "changer de ville", "change la ville",
    "change my city", "change the city", "change my town", NULL
};
/* Words before the name in an answer ("j'habite à Evere"), skipped; articles are kept ("Le Havre") */
static const char *const s_answer_prefix[] = {
    "j habite à", "j habite en", "j habite au", "j habite aux", "j habite", "ma ville est", "mon pays est",
    "c est", "en", "au", "aux", "à", "i live in", "my city is", "my country is", "it is", "it s", "in", NULL
};
static const char *const s_yes[] = {
    "oui", "ouais", "yes", "yeah", "yep", "exact", "exactement", "je confirme", "confirme", "confirmé",
    "c est ça", "c est bon", "d accord", "ok", "okay", "bien sûr", "absolument", "correct", "sure", "right",
    "affirmatif", NULL
};

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
static const char *const s_courtesy[] = { "s il te plait", "s il te plaît", "s il vous plait", "s il vous plaît",
                                          "maintenant", "please", "now", NULL };

/* Languages offered, with the names they may be called by (in both languages, transcription variants) */
static const struct {
    const char *code;
    const char *names[6];
} s_languages[] = {
    { "fr", { "français", "francais", "french", NULL } },
    { "en", { "anglais", "english", "inglish", "l anglais", "angle", NULL } },     /* "angle": heard for "anglais" */
};

/* Numbers 1..9 as said (digits, French and English words, ordinals); "un"/"une" are articles, see find_model() */
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
};

typedef enum {
    ASKED_NOTHING,
    ASKED_LANGUAGE,
    ASKED_MODEL,
    ASKED_COUNTRY,
    ASKED_CITY,
    CONFIRM_COUNTRY,        /* "Confirmes-tu ... ?": s_pending holds the country code */
    CONFIRM_CITY,           /* s_pending holds the town */
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
    return rest[n] == '\0' || (rest[n] == ' ' && starts_with(rest + n + 1, s_courtesy, NULL));
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
static const char *const s_one_alone[] = { "un", "une", "le un", "le une", "numéro un", "numero un", "le numéro un",
                                           NULL };

/* Model number (1-based) said in the text: a word of the model's name ("sonnet"), else a number; 0 if none */
static int find_model(const char *norm)
{
    // Names: the words of the label made of letters only, except the provider ("haiku", "sonnet", "sol"...)
    for (size_t i = 0; i < llm_model_count; i++) {
        char label[48];
        app_text_normalize(llm_models[i].label, label, sizeof(label));
        for (char *word = strtok(label, " "); word; word = strtok(NULL, " ")) {
            bool letters = true;
            for (const char *c = word; *c; c++) {
                letters &= !isdigit((unsigned char)*c);
            }
            if (letters && strcmp(word, "claude") != 0 && strcmp(word, "gpt") != 0 && has_word(norm, word)) {
                return (int)i + 1;
            }
        }
    }
    for (const char *const *one = s_one_alone; *one; one++) {
        if (strcmp(norm, *one) == 0) {
            return 1;
        }
    }
    for (size_t i = 0; i < sizeof(s_numbers) / sizeof(s_numbers[0]); i++) {
        for (size_t j = 0; j < sizeof(s_numbers[0]) / sizeof(s_numbers[0][0]) && s_numbers[i][j]; j++) {
            if (has_word(norm, s_numbers[i][j])) {
                return (int)i + 1 <= (int)llm_model_count ? (int)i + 1 : 0;
            }
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

/* "Voici les modèles disponibles. 1, Haiku, celui que j'utilise. 2, Sonnet. ... Dites son numéro." */
static char *model_list(void)
{
    const app_lang_t *lang = app_lang_get();
    const char *current = settings_get_parameter()->chat_model;
    char *out = malloc(CMD_REPLY_SIZE);
    if (!out) {
        return NULL;
    }
    strlcpy(out, lang->model_ask_intro, CMD_REPLY_SIZE);
    for (size_t i = 0; i < llm_model_count; i++) {
        size_t n = strlen(out);
        snprintf(out + n, CMD_REPLY_SIZE - n, " %u, %s%s.", (unsigned)(i + 1), llm_models[i].spoken,
                 strcmp(llm_models[i].id, current) == 0 ? lang->model_current : "");
    }
    strlcat(out, " ", CMD_REPLY_SIZE);
    strlcat(out, lang->model_ask_question, CMD_REPLY_SIZE);
    return out;
}

static voice_cmd_t choose_model(const char *norm, char **reply)
{
    const app_lang_t *lang = app_lang_get();
    int number = find_model(norm);
    char *text = NULL;
    if (number == 0) {
        asprintf(&text, lang->model_unknown, (int)llm_model_count);
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
        asprintf(&text, lang->model_unknown, (int)llm_model_count);
        *reply = text ? text : reply_dup("");
        return VOICE_CMD_ANSWERED;
    }
    ESP_LOGI(TAG, "model %s saved", model->id);
    assistant_set_model(model->id);
    asprintf(&text, lang->model_switched, model->label);
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
    if (starts_with(norm, s_answer_prefix, &len)) {
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
    for (const char *const *word = s_yes; *word && !yes; word++) {
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

void voice_cmd_reply_spoken(void)
{
    if (s_asked != ASKED_NOTHING) {
        s_asked_us = esp_timer_get_time();
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
    if (recent && asked == ASKED_MODEL) {
        return choose_model(norm, reply);
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

    size_t len = 0;
    if (starts_with(norm, s_new_conversation, &len)) {
        // Alone (or with "s'il te plaît"): answered here; followed by a question: the model answers it
        const char *rest = norm + len + (norm[len] == ' ');
        if (!*rest || starts_with(rest, s_courtesy, NULL)) {
            *reply = reply_dup(app_lang_get()->new_conversation);
            return VOICE_CMD_NEW_CONVERSATION;
        }
        return VOICE_CMD_NEW_CONVERSATION_ASK;
    }
    if (starts_with(norm, s_change_lang, NULL)) {
        // "Change la langue en anglais": the language may already be given
        if (find_language(norm)) {
            return choose_language(norm, reply);
        }
        s_asked = ASKED_LANGUAGE;
        s_asked_us = esp_timer_get_time();
        *reply = reply_dup(app_lang_get()->lang_ask);
        return VOICE_CMD_ANSWERED;
    }
    if (starts_with(norm, s_change_model, &len)) {
        // "Change de modèle pour le 3": the model may already be given
        if (find_model(norm + len)) {
            return choose_model(norm + len, reply);
        }
        s_asked = ASKED_MODEL;
        s_asked_us = esp_timer_get_time();
        *reply = model_list();
        return VOICE_CMD_ANSWERED;
    }
    if (starts_with(norm, s_open_settings, NULL)) {
        return VOICE_CMD_OPEN_SETTINGS;     // shown at once, nothing spoken
    }
    if (starts_with(norm, s_change_country, NULL)) {
        s_asked = ASKED_COUNTRY;
        s_asked_us = esp_timer_get_time();
        *reply = reply_dup(app_lang_get()->country_ask);
        return VOICE_CMD_ANSWERED;
    }
    if (starts_with(norm, s_change_city, NULL)) {
        s_asked = ASKED_CITY;
        s_asked_us = esp_timer_get_time();
        *reply = reply_dup(app_lang_get()->city_ask);
        return VOICE_CMD_ANSWERED;
    }
    // Shortcuts: only when the rest is just a language name or a model number ("parle plus fort" is not one)
    if (starts_with(norm, s_lang_shortcut, &len) && is_language_only(norm[len] ? norm + len + 1 : norm + len)) {
        return choose_language(norm + len, reply);
    }
    if (starts_with(norm, s_model_shortcut, &len) && is_model_only(norm[len] ? norm + len + 1 : norm + len)) {
        return choose_model(norm + len, reply);
    }
    return VOICE_CMD_NONE;
}
