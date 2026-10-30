/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * OpenAI transcription (gpt-live-transcribe): Realtime API, transcription-only session over a WebSocket kept open
 * (connected at boot), each listening session a turn on it:
 *
 *   input_audio_buffer.clear -> input_audio_buffer.append (24 kHz) ... -> input_audio_buffer.commit
 *   -> conversation.item.input_audio_transcription.delta ... .completed
 *
 * gpt-live-transcribe has no server-side VAD: the end of speech is detected on the device. Billing is by audio
 * duration: audio is only sent during a turn, an idle connection sends none.
 */

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "cJSON.h"
#include "stt_backend.h"
#include "stt_stream.h"

/* "https://api.openai.com/v1/" (CONFIG_ASSISTANT_OPENAI_URL) -> "wss://api.openai.com/v1/realtime?intent=..." */
static char *openai_uri(const char *model, const char *language)
{
    const char *host_path = strstr(CONFIG_ASSISTANT_OPENAI_URL, "://");
    host_path = host_path ? host_path + 3 : CONFIG_ASSISTANT_OPENAI_URL;
    char *uri = NULL;
    asprintf(&uri, "wss://%srealtime?intent=transcription", host_path);
    return uri;
}

static char *openai_session_update(const char *model, const char *language)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "session.update");
    cJSON *session = cJSON_AddObjectToObject(msg, "session");
    cJSON_AddStringToObject(session, "type", "transcription");
    cJSON *input = cJSON_AddObjectToObject(cJSON_AddObjectToObject(session, "audio"), "input");
    cJSON *format = cJSON_AddObjectToObject(input, "format");
    cJSON_AddStringToObject(format, "type", "audio/pcm");
    cJSON_AddNumberToObject(format, "rate", 24000);
    cJSON *transcription = cJSON_AddObjectToObject(input, "transcription");
    cJSON_AddStringToObject(transcription, "model", model);
    if (language) {
        cJSON *languages = cJSON_AddArrayToObject(transcription, "languages");
        cJSON_AddItemToArray(languages, cJSON_CreateString(language));
    }
    /* The end of speech is detected on the device (gpt-live-transcribe has no server VAD) */
    cJSON_AddNullToObject(input, "turn_detection");
    char *text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    return text;
}

static void openai_on_message(const cJSON *json, const char *t)
{
    if (strcmp(t, "conversation.item.input_audio_transcription.delta") == 0) {
        const cJSON *delta = cJSON_GetObjectItem(json, "delta");
        if (cJSON_IsString(delta)) {
            stt_backend_delta(delta->valuestring);
        }
    } else if (strcmp(t, "conversation.item.input_audio_transcription.completed") == 0) {
        // Billed: tokens of audio heard (in) and of text written (out), or the duration of the audio heard
        const cJSON *usage = cJSON_GetObjectItem(json, "usage");
        const cJSON *in = cJSON_GetObjectItem(usage, "input_tokens");
        const cJSON *out = cJSON_GetObjectItem(usage, "output_tokens");
        const cJSON *seconds = cJSON_GetObjectItem(usage, "seconds");
        const cJSON *transcript = cJSON_GetObjectItem(json, "transcript");
        stt_backend_final(cJSON_IsString(transcript) ? transcript->valuestring : "",
                          cJSON_IsNumber(in) ? in->valueint : 0, cJSON_IsNumber(out) ? out->valueint : 0,
                          cJSON_IsNumber(seconds) ? seconds->valuedouble : -1);
    } else if (strcmp(t, "conversation.item.input_audio_transcription.failed") == 0 || strcmp(t, "error") == 0) {
        const cJSON *error = cJSON_GetObjectItem(json, "error");
        const cJSON *code = cJSON_GetObjectItem(error, "code");
        const cJSON *message = cJSON_GetObjectItem(error, "message");
        stt_backend_failed(cJSON_IsString(code) ? code->valuestring : NULL,
                           cJSON_IsString(message) ? message->valuestring : NULL);
    }
}

const stt_backend_t stt_backend_openai = {
    .name = "openai",
    .rate = 24000,
    .uri = openai_uri,
    .session_update = openai_session_update,
    .append_type = "input_audio_buffer.append",
    .clear_type = "input_audio_buffer.clear",
    .commit_type = "input_audio_buffer.commit",
    .session_per_turn = false,
    .on_message = openai_on_message,
};
