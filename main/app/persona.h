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

/**
 * @brief Install the files of the languages of the firmware when missing; the PERSONA.TXT of an older firmware
 *        (old_persona, NVS key Persona) becomes the [personality] of the file of the language given. SPIFFS mounted.
 * @return true when old_persona was moved (the NVS key can be erased)
 */
bool persona_init(const char *lang_code, const char *old_persona);

/** Read the file of a language (the English one for a language without its own file) */
void persona_load(const char *lang_code);

/** A section of the language loaded ("" if empty) */
const char *persona_get(persona_section_t section);

#ifdef __cplusplus
}
#endif
