/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include "esp_err.h"

#define SSID_SIZE 32
#define PASSWORD_SIZE 64
/* API keys: OpenAI keys are 164 characters today, Anthropic ones ~110; keep room for longer ones */
#define KEY_SIZE 256
#define URL_SIZE 64
#define LANG_SIZE 8
#define MODEL_SIZE 48
#define CLAUDE_KEY_SIZE 256

typedef struct {
    char ssid[SSID_SIZE];             /* SSID of target AP. */
    char password[PASSWORD_SIZE];     /* Password of target AP. */
    char key[KEY_SIZE];               /* OpenAI key. */
    char url[URL_SIZE];               /* OpenAI Base url. */
    char lang[LANG_SIZE];             /* Assistant language (ISO-639-1). */
    char claude_key[CLAUDE_KEY_SIZE]; /* Anthropic key, empty if not configured. */
    char claude_model[MODEL_SIZE];    /* Claude model. */
} sys_param_t;

esp_err_t settings_factory_reset(void);
esp_err_t settings_read_parameter_from_nvs(void);
sys_param_t *settings_get_parameter(void);
