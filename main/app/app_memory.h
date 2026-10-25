/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Long-term memory: notes the user asks the assistant to remember ("souviens-toi que ..."), kept in the NVS
 * key "Memory" (MEMORY.TXT in the configuration mode) and given to the assistant with every question.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MEMORY_CMD_NONE,        /* not a memory command: ask the model */
    MEMORY_CMD_CHANGED,     /* the notes changed: the instructions must be rebuilt */
    MEMORY_CMD_ANSWERED,    /* answered without change (list, nothing found, memory full) */
} memory_cmd_t;

/**
 * @brief Lowercase a sentence for matching: letters and digits kept (accents too, Latin-1 capitals
 *        lowercased), any other run of characters becomes one space, no space at the ends.
 */
void app_text_normalize(const char *text, char *out, size_t size);

/** Set the notes read from NVS at start-up (copied), NULL or "" for none */
void app_memory_init(const char *notes);

/** Current notes, one per line ("- note (25/10/26)"), "" if none */
const char *app_memory_notes(void);

/**
 * @brief Handle "souviens-toi que ...", "oublie que ...", "oublie tout", "dis-moi tout ce que tu as mémorisé"
 *        (and their English equivalents).
 *
 * @param text  Transcript of the question
 * @param reply Sentence to speak (to be freed with free()) when the result is not MEMORY_CMD_NONE
 */
memory_cmd_t app_memory_handle(const char *text, char **reply);

#ifdef __cplusplus
}
#endif
