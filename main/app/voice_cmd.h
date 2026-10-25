/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Settings changed by voice, answered on the device:
 * - language: "change la langue" (the assistant asks which one), or directly "parle anglais", "speak French";
 * - model: "change de modèle" (the assistant lists the numbered models), or directly "utilise le modèle 3";
 * - country and town: "change mon pays", "change ma ville" (asked, then confirmed before saving).
 * The new setting is saved and applied at once, without restarting.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_CMD_NONE,         /* not a settings command: ask the model */
    VOICE_CMD_ANSWERED,     /* question asked, setting changed, or nothing to change */
} voice_cmd_t;

/**
 * @param text  Transcript of the question
 * @param reply Sentence to speak (allocated, to be freed) when the result is not VOICE_CMD_NONE
 */
voice_cmd_t voice_cmd_handle(const char *text, char **reply);

/** A reply has been spoken: the delay to answer a question starts now (called when the question ends) */
void voice_cmd_reply_spoken(void);

#ifdef __cplusplus
}
#endif
