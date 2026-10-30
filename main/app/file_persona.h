/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Who the assistant is, per language: /spiffs/persona_<code>.txt (PERSO_FR.TXT, PERSO_EN.TXT on the configuration
 * drive), sections [identity], [personality], [instructions] and [voice] (French names accepted). Installed from the
 * firmware's version (main/persona/) when missing or emptied; a missing section takes the firmware's text.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PERSONA_IDENTITY = 0,
    PERSONA_PERSONALITY,
    PERSONA_INSTRUCTIONS,
    PERSONA_VOICE,
    PERSONA_SECTIONS,
} persona_section_t;

/** Install the files of the languages of the firmware when missing or emptied (SPIFFS mounted) */
void persona_init(void);

/**
 * @brief The file of a language added to Languages of CONFIG.INI, made from the English one of the firmware (to be
 *        translated) when it does not exist yet
 * @return true when it was made
 */
bool persona_create(const char *code, const char *name);

/** Read the file of a language (the English one for a language without its own file) */
void persona_load(const char *lang_code);

/** A section of the language loaded ("" if empty) */
const char *persona_get(persona_section_t section);

#ifdef __cplusplus
}
#endif
