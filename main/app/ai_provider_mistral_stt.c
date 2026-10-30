/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Mistral AI transcription (Voxtral Realtime, voxtral-mini-transcribe-realtime-3-0): a WebSocket session per
 * question, opened when listening starts (the audio is buffered meanwhile) and over once transcribed:
 *
 *   wss://api.mistral.ai/v1/audio/transcriptions/realtime?model=...
 *   session.update (pcm_s16le, 16 kHz) -> input_audio.append ... -> input_audio.end
 *   -> transcription.text.delta ... -> transcription.done
 *
 * Billed by the minute of audio ($0.008): counted from the duration of the audio sent. The language in use is
 * given in session.update (else the model guesses it, and may hear another one).
 */

#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "stt_backend.h"
#include "stt_stream.h"

static char *mistral_uri(const char *model)
{
    char *uri = NULL;
    asprintf(&uri, "wss://api.mistral.ai/v1/audio/transcriptions/realtime?model=%s", model);
    return uri;
}

static char *mistral_session_update(const char *model, const char *language)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "session.update");
    cJSON *session = cJSON_AddObjectToObject(msg, "session");
    cJSON *format = cJSON_AddObjectToObject(session, "audio_format");
    cJSON_AddStringToObject(format, "encoding", "pcm_s16le");
    cJSON_AddNumberToObject(format, "sample_rate", 16000);
    // The language in use ("fr"): left to the model, it may hear another one
    if (language && strlen(language) == 2) {
        cJSON_AddStringToObject(session, "language", language);
    }
    char *text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    return text;
}

static void mistral_on_message(const cJSON *json, const char *t)
{
    if (strcmp(t, "transcription.text.delta") == 0) {
        const cJSON *text = cJSON_GetObjectItem(json, "text");
        if (cJSON_IsString(text)) {
            stt_backend_delta(text->valuestring);
        }
    } else if (strcmp(t, "transcription.done") == 0) {
        // The whole text when given, else the deltas; billed by the duration of the audio sent
        const cJSON *text = cJSON_GetObjectItem(json, "text");
        stt_backend_final(cJSON_IsString(text) && text->valuestring[0] ? text->valuestring : NULL, 0, 0, -1);
    } else if (strcmp(t, "error") == 0) {
        // {"type": "error", "error": {"message", "code"}} or {"type": "error", "message"}
        const cJSON *error = cJSON_GetObjectItem(json, "error");
        const cJSON *message = cJSON_IsString(error) ? error : cJSON_GetObjectItem(error, "message");
        if (!cJSON_IsString(message)) {
            message = cJSON_GetObjectItem(json, "message");
        }
        const cJSON *code = cJSON_GetObjectItem(error, "code");
        char code_text[16] = "";
        if (cJSON_IsNumber(code)) {
            snprintf(code_text, sizeof(code_text), "%d", code->valueint);
        }
        stt_backend_failed(cJSON_IsString(code) ? code->valuestring : code_text,
                           cJSON_IsString(message) ? message->valuestring : NULL);
    }
}

const stt_backend_t stt_backend_mistral = {
    .name = "mistral",
    .rate = 16000,
    .uri = mistral_uri,
    .session_update = mistral_session_update,
    .append_type = "input_audio.append",
    .clear_type = NULL,
    .commit_type = "input_audio.end",
    .session_per_turn = true,
    .on_message = mistral_on_message,
};
