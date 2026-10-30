/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A preset voice of Voxtral TTS */
typedef struct {
    char slug[40];      /* voice_id: "fr_marie_neutral" */
    char name[40];      /* "Marie - Neutral" */
} mistral_voice_t;

/**
 * @brief The preset voices of a language, read from Mistral (GET /v1/audio/voices, about a second).
 *
 * @param lang ISO-639-1 code ("fr"): the voices whose slug or languages start with it
 * @return How many were written to voices (at most max), 0 if the list could not be read
 */
size_t mistral_tts_voices(const char *api_key, const char *lang, mistral_voice_t *voices, size_t max);

#ifdef __cplusplus
}
#endif
