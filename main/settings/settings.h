/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define SSID_SIZE 32
#define PASSWORD_SIZE 64
/* API keys: OpenAI keys are 164 characters today, Anthropic ones ~110; keep room for longer ones */
#define KEY_SIZE 256
#define URL_SIZE 64
#define LANG_SIZE 8
#define MODEL_SIZE 48
#define CLAUDE_KEY_SIZE 256
#define GEMINI_KEY_SIZE 128
#define OPENROUTER_KEY_SIZE 128
#define XAI_KEY_SIZE 128
#define MISTRAL_KEY_SIZE 128
#define PERPLEXITY_KEY_SIZE 128
#define DEEPGRAM_KEY_SIZE 128
#define VOICE_SIZE 24
#define COUNTRY_SIZE 4
#define CITY_SIZE 48
#define HOMEY_URL_SIZE 96
#define HOMEY_KEY_SIZE 96

typedef struct {
    char ssid[SSID_SIZE];             /* SSID of target AP. */
    char password[PASSWORD_SIZE];     /* Password of target AP. */
    char key[KEY_SIZE];               /* OpenAI key. */
    char url[URL_SIZE];               /* OpenAI Base url. */
    char lang[LANG_SIZE];             /* Assistant language (ISO-639-1). */
    char claude_key[CLAUDE_KEY_SIZE]; /* Anthropic key, empty if not configured. */
    char gemini_key[GEMINI_KEY_SIZE]; /* Gemini key (Google AI Studio), empty if not configured. */
    char openrouter_key[OPENROUTER_KEY_SIZE];       /* OpenRouter key, empty if not configured. */
    char xai_key[XAI_KEY_SIZE];       /* xAI key (Grok models), empty if not configured. */
    char mistral_key[MISTRAL_KEY_SIZE];             /* Mistral AI key, empty if not configured. */
    char perplexity_key[PERPLEXITY_KEY_SIZE];       /* Perplexity key (Sonar), empty if not configured. */
    char deepgram_key[DEEPGRAM_KEY_SIZE];           /* Deepgram key (Nova, Aura speech), empty if not configured. */
    char chat_model[MODEL_SIZE];      /* Model answering the questions (Claude, GPT, Gemini, Grok or OpenRouter). */
    int scroll_cps;                   /* Reading speed of the answer on screen (characters per second). */
    int screen_off_s;                 /* Screen off after this long without presence (radar dock), 0: never. */
    int radio_treble;                 /* Treble of the radio, in dB (-12..6; negative: softer, 0: unchanged). */
    bool radio_resume;                /* A radio interrupted by a conversation plays again afterwards. */
    int radio_level;                  /* Level the radio stations are brought to, in dBFS (-30..-6). */
    bool show_clock;                  /* Time and date shown while the assistant sleeps. */
    bool sleep_after_cmd;             /* Back to sleep after a voice command handled locally (no follow-up). */
    bool read_lists;                  /* Lists of the voice commands read aloud, not only shown (after "::"). */
    char languages[160];              /* Languages offered: "FR:Français, EN:English" */
    char currency[8];                 /* Of the prices of AGENTS.INI and of the costs: "EUR", "USD"... */
    int free_models;                  /* How many of them are offered (0..5, the most used), besides "openrouter/free". */
    int end_speech_ms;                /* Silence after which the question is considered finished. */
    bool scroll_smooth;               /* Answer scrolls continuously (true) or one line at a time (false). */
    char voice[VOICE_SIZE];           /* Voice reading the answers: its name in [voices] of AGENTS.INI (Marin, Lea...). */
    char stt_model[MODEL_SIZE];       /* Transcription model ([stt] of AGENTS.INI). */
    char tts_model[MODEL_SIZE];       /* Speech model ([tts] of AGENTS.INI). */
    char country[COUNTRY_SIZE];       /* Where the user lives: ISO 3166-1 alpha-2 code ("BE"), "" if unknown. */
    char city[CITY_SIZE];             /* Where the user lives: town ("Evere"), "" if unknown. */
    char homey_url[HOMEY_URL_SIZE];   /* MCP server of a Homey Pro on the local network, "" if none. */
    char homey_key[HOMEY_KEY_SIZE];   /* Its API key. */
    char confirm_off[64];             /* Homey classes confirmed before being turned off ("socket"), "" for none. */
    int volume;                       /* Speaker volume 0..100, changed by voice ("parle plus fort"). */
    char *memory;                     /* Notes it was asked to remember at start-up (allocated), NULL if none. */
} sys_param_t;

esp_err_t settings_factory_reset(void);

/**
 * @brief A text of MEMORY.TXT without its comment lines (starting with '#'), trimmed.
 *
 * @return Allocated (to be freed), "" when nothing is left; NULL only when out of memory
 */
char *settings_text_without_comments(const char *text);
esp_err_t settings_read_parameter_from_nvs(void);
sys_param_t *settings_get_parameter(void);

/**
 * @brief Store a configuration value (same keys as CONFIG.INI, e.g. "Language", "Chat_model").
 *
 * Read again at the next start: the caller restarts the device.
 */
esp_err_t settings_save_str(const char *key, const char *value);

/** Erase a configuration value of NVS */
esp_err_t settings_erase_key(const char *key);

/**
 * @brief Soft reset: the settings of CONFIG.INI back to their defaults, except the Wi-Fi, the API keys and Homey
 *        (Persona, Memory and the comments of CONFIG.INI are kept too). Applied at the next start (the caller
 *        restarts the device).
 */
esp_err_t settings_restore_defaults(void);

/**
 * @brief Factory reset: every setting erased (Wi-Fi, keys, Persona, Memory included).
 */
esp_err_t settings_factory_wipe(void);
