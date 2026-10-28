/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "llm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LANG_CODE_SIZE 8
/* Languages offered, at most (Languages of CONFIG.INI) */
#define LANG_MAX       12
#define LANG_NAME_SIZE 24

typedef struct {
    const char *code;            /* ISO-639-1 code passed to the transcription API */
    const char *listening;
    const char *heard;              /* end of the question, until its transcript arrives */
    const char *thinking;           /* transcript received, until the answer starts */
    const char *sorry;
    const char *wifi_connecting;
    const char *wifi_failed;
    const char *system_prompt;   /* System message sent with every chat request */
    const char *tts_instructions;   /* Speaking style given to the speech synthesis */
    const char *stt_error;          /* Spoken when the question could not be transcribed */
    /* Long-term memory ("souviens-toi que ...") */
    const char *memory_header;      /* Introduces the notes in the instructions */
    const char *memory_saved;
    const char *memory_full;
    const char *memory_forgotten;
    const char *memory_not_found;
    const char *memory_cleared;
    const char *memory_empty;
    const char *memory_list_intro;
    /* Language changed by voice ("change la langue") */
    const char *lang_ask;
    const char *lang_already;
    const char *lang_unknown;
    const char *lang_switched;      /* spoken in the new language, before the restart */
    /* Model changed by voice ("change de modèle"): the list of numbered models, then the choice */
    const char *models_first;       /* "liste tes modèles", first provider: %s provider, %s its models */
    const char *models_next;        /* next providers: %s provider, %s its models */
    const char *models_free;        /* %s: the free OpenRouter models */
    const char *and_word;           /* before the last name of a list: " et " */
    const char *language_added;     /* %s: a language added to Languages, before the configuration mode */
    const char *lang_untranslated;  /* %s: code (FR), %s: language name: texts added in English to LANG_<CODE>.TXT */
    const char *lang_bad_format;    /* %s: code, %s: keys whose %s, %d... differ from the English texts */
    /* How this language calls the languages, said to change ("de: allemand, nl: néerlandais/hollandais") */
    const char *language_names;
    /* Numbers 1, 2, 3... as said to choose a model or a voice ("premier/première, deux/deuxième, ...") */
    const char *numbers;
    const char *test_question;      /* "teste les modèles": the question asked to every model */
    const char *test_start;         /* %d: number of models */
    const char *test_running;
    const char *test_unavailable_short; /* on screen, after the model's name */
    const char *test_result;        /* %s spoken name, %s time ("1,2 secondes") */
    const char *test_unavailable;   /* %s spoken name */
    const char *test_done;          /* %s fastest */
    const char *test_none;
    const char *exclude_ask;        /* %s spoken name, %s its mean time: exclude it? */
    const char *exclude_yes;        /* before the next question */
    const char *exclude_no;
    const char *exclude_finished;   /* after the last answer */
    const char *fastest_models;     /* %s: the 3 fastest models of the last tests, with their mean time */
    const char *fastest_none;       /* no test yet: run one? */
    const char *test_declined;      /* "no" to that question */
    /* Radio ("joue la radio X", "joue du jazz", "suivante", "stop", "qu'est-ce qui passe") */
    const char *radio_playing;      /* %s: station */
    const char *radio_not_found;    /* %s: what was asked */
    const char *radio_unreachable;
    const char *radio_ask;
    const char *radio_stopped;
    const char *radio_last;
    const char *radio_first;
    const char *radio_none;
    const char *radio_what;         /* %s: station */
    const char *radio_what_title;   /* %s: station, %s: title */
    const char *radio_genres;       /* "classique/musique classique: classical, ...": words said -> Radio-Browser tag */
    const char *radio_fav_added;    /* %s: station */
    const char *radio_fav_already;  /* %s: station */
    const char *radio_fav_removed;  /* %s: station */
    const char *radio_fav_not;      /* %s: station */
    const char *radio_fav_none;
    const char *radio_fav_full;
    /* Radio screen (tap on the volume of the sleep screen) */
    const char *radio_title;
    const char *radio_volume_label;     /* %d: volume */
    const char *radio_treble_label;
    const char *radio_nothing;
    const char *radio_fav_empty;
    char decimal_mark;              /* ',' or '.' */
    const char *second_one;         /* "seconde", below second_one_below_ms */
    const char *seconds;
    int second_one_below_ms;
    const char *provider_ask;       /* "change de modèle": which provider first */
    const char *provider_unknown;
    const char *model_ask_intro;    /* %s: provider ("Anthropic"), its numbered models follow */
    const char *model_current;      /* after the current model in the list */
    const char *model_ask_question;
    const char *model_already;      /* %s: model name */
    const char *model_unknown;      /* %d: number of models */
    const char *model_switched;     /* %s: model name */
    const char *voice_ask_intro;    /* "change de voix": the numbered voices follow */
    const char *voice_current;      /* after the current voice in the list */
    const char *voice_ask_question;
    const char *voice_already;      /* %s: voice name */
    const char *voice_unknown;      /* %d: number of voices */
    /* Country and town changed by voice ("change mon pays", "change ma ville"), confirmed before saving */
    const char *country_ask;
    const char *city_ask;
    const char *country_confirm;    /* %s: country name */
    const char *city_confirm;       /* %s: town */
    const char *country_unknown;
    const char *place_saved;
    const char *place_cancelled;
    const char *location_line;      /* in the instructions, %s: "Evere, BE" */
    const char *config_need_pc;     /* "édite la config" without a PC connected */
    const char *config_opening;     /* "édite la config": restarting in configuration mode */
    const char *restarting;         /* "redémarre" */
    const char *clock_shown;        /* "affiche l'heure" */
    const char *costs_title;        /* page of the costs (tap on the cost, top right) */
    const char *costs_conversation; /* its rows: since the last wake-up, today, this month */
    const char *costs_today;
    const char *costs_month;
    const char *costs_total;        /* its last column */
    const char *clock_hidden;       /* "cache l'heure" */
    const char *new_conversation;   /* "nouvelle discussion": the conversation is cleared */
    const char *voice_choppy;       /* shown after a reply whose speech was cut (slow connection) */
    const char *home_line;          /* in the instructions when a Homey is set: ask it, do not answer from memory */
    /* Day abbreviations of the clock under the sleeping robot, Sunday first */
    const char *weekdays[7];
    /* Current date and time given with each question: %s day, %d day of month, %s month, %d year, %02d:%02d */
    const char *now_format;
    const char *weekdays_long[7];   /* Sunday first */
    const char *months[12];

    /* Chat errors, shown and spoken as the answer (indexed by provider and llm_error_t, LLM_OK unused) */
    const char *llm_errors[LLM_PROVIDER_COUNT][LLM_ERR_OTHER + 1];

    /* Static screens generated by SquareLine; NULL keeps the generated (English) text */
    const char *steps_title;
    const char *steps_content;
    const char *settings_title;
    const char *settings_language;
    const char *settings_model;
    const char *settings_voice;
    const char *voice_sample;       /* Spoken with a voice just chosen, %s = its name */
    const char *settings_save;      /* Save button, enabled once the language or the model is changed */
    const char *settings_saved;     /* Shown while the device restarts after saving */
    const char *soft_title;         /* Configuration screen, icon "soft reset": its confirmation */
    const char *soft_text;
    const char *soft_yes;
    const char *factory_title;      /* icon "factory reset": its confirmation */
    const char *factory_text;
    const char *factory_yes;
    const char *restore_no;         /* both: cancel */
    const char *restore_done;       /* on the OK button while the device restarts */
    const char *reset_title;
    const char *reset_content;
} app_lang_t;

/**
 * @brief Select the assistant language.
 *
 * Unknown codes keep the English UI texts but are still used for transcription,
 * and the assistant is asked to answer in the user's language.
 */
void app_lang_set(const char *code);

const app_lang_t *app_lang_get(void);

/**
 * Problems found in the file of the language in use when it was read: texts missing (added in English, to be
 * translated) or whose %s, %d... differ from the English ones. The message to show (in that language), false if none.
 * Reported once.
 */
bool app_lang_problems(char *out, size_t size);

/**
 * @brief The languages offered, from Languages of CONFIG.INI ("FR:Français, EN:English"); before app_lang_set().
 *        Without a valid one, French and English.
 */
void app_lang_init(const char *languages);

/** Languages offered (their files are read too): count, code of each ("fr", "en"), name ("Français") */
size_t app_lang_count(void);
const char *app_lang_code(size_t index);
const char *app_lang_name(size_t index);

/**
 * @brief The file of a language added to Languages of CONFIG.INI, made from the English one of the firmware (to be
 *        translated) when it does not exist yet
 * @return true when it was made
 */
bool app_lang_create(const char *code, const char *name);

/** Texts of a supported language (e.g. to confirm a change in the new language), NULL if not supported */
const app_lang_t *app_lang_find(const char *code);

#ifdef __cplusplus
}
#endif
