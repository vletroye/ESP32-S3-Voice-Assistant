/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Countries known by voice: /spiffs/country.txt (COUNTRY.TXT of the configuration drive), their names per language */
#define COUNTRY_FILE        "/spiffs/country.txt"
/* The file before a soft reset: the columns of the languages added are taken from it at start-up */
#define COUNTRY_OLD_FILE    "/spiffs/country.old"

/** Read the countries (installed when missing); a column added for each language offered without one. After app_lang_init */
void country_init(void);

/** The country named in a normalized sentence (its names in the language in use or in English, or its code alone):
 *  its 2-letter code in code; false if none */
bool country_find(const char *norm, char *code, size_t size);

/** The name of a country in the language in use (English when it has none, the code when unknown) */
void country_name(const char *code, char *out, size_t size);

#ifdef __cplusplus
}
#endif
