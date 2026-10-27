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
    char chat_model[MODEL_SIZE];      /* Model answering the questions (Claude, GPT, Gemini or OpenRouter). */
    int scroll_cps;                   /* Reading speed of the answer on screen (characters per second). */
    int screen_off_s;                 /* Screen off after this long without presence (radar dock), 0: never. */
    bool show_clock;                  /* Time and date shown while the assistant sleeps. */
    bool sleep_after_cmd;             /* Back to sleep after a voice command handled locally (no follow-up). */
    char currency[8];                 /* Of the prices of AGENTS.INI and of the costs: "EUR", "USD"... */
    bool openrouter_free;             /* Free OpenRouter models read from its live list (once a day). */
    int free_models;                  /* How many of them are offered (1..15), besides "openrouter/free". */
    int end_speech_ms;                /* Silence after which the question is considered finished. */
    bool scroll_smooth;               /* Answer scrolls continuously (true) or one line at a time (false). */
    char voice[VOICE_SIZE];           /* OpenAI voice reading the answers (marin, cedar...). */
    char country[COUNTRY_SIZE];       /* Where the user lives: ISO 3166-1 alpha-2 code ("BE"), "" if unknown. */
    char city[CITY_SIZE];             /* Where the user lives: town ("Evere"), "" if unknown. */
    char homey_url[HOMEY_URL_SIZE];   /* MCP server of a Homey Pro on the local network, "" if none. */
    char homey_key[HOMEY_KEY_SIZE];   /* Its API key. */
    char confirm_off[64];             /* Homey classes confirmed before being turned off ("socket"), "" for none. */
    int volume;                       /* Speaker volume 0..100, changed by voice ("parle plus fort"). */
    char *persona;                    /* Context always given to the assistant (allocated), NULL if none. */
    char *memory;                     /* Notes it was asked to remember at start-up (allocated), NULL if none. */
} sys_param_t;

esp_err_t settings_factory_reset(void);

/**
 * @brief A text of PERSONA.TXT or MEMORY.TXT without its comment lines (starting with '#'), trimmed.
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

/** CONFIG.BKP of the configuration drive: CONFIG.INI (and PERSONA.TXT, MEMORY.TXT) saved by the factory reset */
#define SETTINGS_BACKUP_FILE    "/spiffs/config.bkp"

/**
 * @brief Soft reset: the settings of CONFIG.INI back to their defaults, except the Wi-Fi, the API keys and Homey
 *        (Persona, Memory and the comments of CONFIG.INI are kept too). Applied at the next start (the caller
 *        restarts the device).
 */
esp_err_t settings_restore_defaults(void);

/**
 * @brief Factory reset: every setting erased (Wi-Fi, keys, Persona, Memory included). CONFIG.INI is first saved to
 *        SETTINGS_BACKUP_FILE when it differs from the firmware's (otherwise an earlier backup is kept); nothing is
 *        erased if that backup fails.
 */
esp_err_t settings_factory_wipe(void);
