/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Deepgram transcription (Nova-2, Nova-3): a live WebSocket session per question, opened when listening starts
 * (the audio is buffered meanwhile) and over once transcribed:
 *
 *   wss://api.deepgram.com/v1/listen?model=nova-3&language=fr&encoding=linear16&sample_rate=16000...
 *   (header "Authorization: Token <key>") -> the audio in binary frames ... -> {"type": "CloseStream"}
 *   -> "Results" (is_final: a piece of the transcript) ... -> "Metadata" (the whole audio transcribed)
 *
 * CloseStream rather than Finalize: the server transcribes what is left, then sends Metadata before closing,
 * a sure end of the turn. Billed by the minute of audio: the duration given by Metadata.
 */

#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "stt_backend.h"
#include "stt_stream.h"

static char *deepgram_uri(const char *model, const char *language)
{
    char *uri = NULL;
    // The language in use ("fr"): without it, English is assumed
    asprintf(&uri, "wss://api.deepgram.com/v1/listen?model=%s&language=%s&encoding=linear16&sample_rate=16000"
             "&channels=1&punctuate=true&smart_format=true", model, language && language[0] ? language : "en");
    return uri;
}

static void deepgram_on_message(const cJSON *json, const char *t)
{
    if (strcmp(t, "Results") == 0) {
        // Only the final pieces (no interim results asked), one after the other, separated by a space
        const cJSON *is_final = cJSON_GetObjectItem(json, "is_final");
        const cJSON *alt = cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(json, "channel"),
                                              "alternatives"), 0);
        const cJSON *transcript = cJSON_GetObjectItem(alt, "transcript");
        if (cJSON_IsTrue(is_final) && cJSON_IsString(transcript) && transcript->valuestring[0]) {
            if (stt_stream_has_text()) {
                stt_backend_delta(" ");
            }
            stt_backend_delta(transcript->valuestring);
        }
    } else if (strcmp(t, "Metadata") == 0) {
        // Everything transcribed (CloseStream): the pieces received; billed by the duration it gives
        const cJSON *duration = cJSON_GetObjectItem(json, "duration");
        stt_backend_final(NULL, 0, 0, cJSON_IsNumber(duration) ? duration->valuedouble : -1);
    } else if (strcmp(t, "Error") == 0) {
        // {"type": "Error", "variant": ..., "description": ...}
        const cJSON *variant = cJSON_GetObjectItem(json, "variant");
        const cJSON *description = cJSON_GetObjectItem(json, "description");
        stt_backend_failed(cJSON_IsString(variant) ? variant->valuestring : NULL,
                           cJSON_IsString(description) ? description->valuestring : NULL);
    }
}

const stt_backend_t stt_backend_deepgram = {
    .name = "deepgram",
    .auth_scheme = "Token",
    .rate = 16000,
    .uri = deepgram_uri,
    .session_update = NULL,
    .append_type = NULL,
    .clear_type = NULL,
    .commit_type = "CloseStream",
    .session_per_turn = true,
    .on_message = deepgram_on_message,
};
