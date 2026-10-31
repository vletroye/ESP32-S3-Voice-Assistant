/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ElevenLabs transcription (Scribe v2 Realtime): a WebSocket session per question, opened when listening starts
 * (the audio is buffered meanwhile) and over once transcribed:
 *
 *   wss://api.elevenlabs.io/v1/speech-to-text/realtime?model_id=scribe_v2_realtime&audio_format=pcm_16000
 *   &commit_strategy=manual&language_code=fr (header "xi-api-key: <key>")
 *   {"message_type": "input_audio_chunk", "audio_base_64": "...", "commit": false, "sample_rate": 16000} ...
 *   -> the same with an empty audio and "commit": true at the end of speech -> "committed_transcript"
 *
 * A segment committed by the service itself (after ~36 s of audio) is kept as a piece of the transcript. Billed by
 * the hour of audio sent ($0.39): counted from its duration.
 */

#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "stt_backend.h"
#include "stt_stream.h"

static char *elevenlabs_uri(const char *model, const char *language)
{
    char *uri = NULL;
    // The language in use ("fr"): left to the service, it may hear another one
    asprintf(&uri, "wss://api.elevenlabs.io/v1/speech-to-text/realtime?model_id=%s&audio_format=pcm_16000"
             "&commit_strategy=manual%s%s", model, language && language[0] ? "&language_code=" : "",
             language && language[0] ? language : "");
    return uri;
}

static void elevenlabs_on_message(const cJSON *json, const char *t)
{
    const cJSON *text = cJSON_GetObjectItem(json, "text");
    const cJSON *error = cJSON_GetObjectItem(json, "error");
    if (strcmp(t, "committed_transcript") == 0) {
        // The pieces one after the other, separated by a space; the one answering the commit ends the turn
        if (cJSON_IsString(text) && text->valuestring[0]) {
            if (stt_stream_has_text()) {
                stt_backend_delta(" ");
            }
            stt_backend_delta(text->valuestring);
        }
        if (stt_backend_awaiting()) {
            stt_backend_final(NULL, 0, 0, -1);
        }
    } else if (cJSON_IsString(error)) {
        // {"message_type": "auth_error" / "quota_exceeded" / ..., "error": "..."}
        stt_backend_failed(t, error->valuestring);
    }
}

const stt_backend_t stt_backend_elevenlabs = {
    .name = "elevenlabs",
    .auth_header = "xi-api-key",
    .rate = 16000,
    .uri = elevenlabs_uri,
    .session_update = NULL,
    .append_type = "input_audio_chunk",
    .type_key = "message_type",
    .audio_key = "audio_base_64",
    .append_extra = ",\"commit\":false,\"sample_rate\":16000",
    .commit_json = "{\"message_type\":\"input_audio_chunk\",\"audio_base_64\":\"\",\"commit\":true,"
                   "\"sample_rate\":16000}",
    .clear_type = NULL,
    .session_per_turn = true,
    .on_message = elevenlabs_on_message,
};
