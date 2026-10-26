/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Settings changed by voice, answered on the device:
 * - language: "change la langue" (the assistant asks which one), or directly "parle anglais", "speak French";
 * - model: "change de modèle" (the assistant asks the provider: Anthropic, OpenAI or Google, then lists its
 *   numbered models), or directly "utilise le modèle 3" (numbered over all models), "change de modèle pour Sonnet";
 * - voice: "change de voix" (the assistant lists the numbered voices), or directly "prends la voix Cedar",
 *   "utilise la voix 2" (the confirmation is spoken with the new voice);
 * - country and town: "change mon pays", "change ma ville" (asked, then confirmed before saving);
 * - "ouvre les paramètres", "open the settings": the Settings screen;
 * - "je veux éditer la config", "édite les fichiers de configuration": restart in configuration mode (CONFIG.INI,
 *   PERSONA.TXT and MEMORY.TXT on a USB drive), only when a PC is connected;
 * - "nouvelle discussion", "j'ai une autre question", "passons à autre chose": the conversation starts afresh
 *   (as after "au revoir"); a question following the phrase is then asked to the model.
 * The new setting is saved and applied at once, without restarting.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_CMD_NONE,         /* not a settings command: ask the model */
    VOICE_CMD_ANSWERED,     /* question asked, setting changed, or nothing to change */
    VOICE_CMD_OPEN_SETTINGS,    /* "ouvre les paramètres": show the Settings screen at once (no reply) */
    VOICE_CMD_NEW_CONVERSATION,     /* "nouvelle discussion" alone: clear the conversation, speak the reply */
    VOICE_CMD_NEW_CONVERSATION_ASK, /* "j'ai une autre question : ..." clear the conversation, ask the model */
    VOICE_CMD_EDIT_CONFIG,  /* "édite la config" with a PC connected: speak the reply, then restart in config mode */
} voice_cmd_t;

/**
 * @param text  Transcript of the question
 * @param reply Sentence to speak (allocated, to be freed) for VOICE_CMD_ANSWERED
 */
voice_cmd_t voice_cmd_handle(const char *text, char **reply);

/** A reply has been spoken: the delay to answer a question starts now (called when the question ends) */
void voice_cmd_reply_spoken(void);

#ifdef __cplusplus
}
#endif
