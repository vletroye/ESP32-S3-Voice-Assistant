/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * A transcription service (ai_provider_<name>_stt.c) for stt_stream.c: the protocol of its Realtime WebSocket.
 * stt_stream.c keeps the connection, the audio and the turns; the service says how to talk to its server and
 * reads its messages, reporting what they say with the stt_backend_...() functions.
 */

#pragma once

#include <stdbool.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

struct stt_backend {
    const char *name;           /* "openai", "mistral", "deepgram" (logs) */
    const char *auth_scheme;    /* of the Authorization header: "Bearer" when NULL, "Token" (Deepgram) */
    const char *auth_header;    /* a header carrying the key alone instead ("xi-api-key", ElevenLabs), else NULL */
    int rate;                   /* sample rate of the audio sent: 24000 (resampled from 16 kHz) or 16000 */
    /* "wss://..." for this model and language (allocated), made at each connection */
    char *(*uri)(const char *model, const char *language);
    /* The session configuration sent once connected (allocated JSON), NULL for none */
    char *(*session_update)(const char *model, const char *language);
    /* message carrying base64 audio: {"type": append_type, "audio": "..."}; NULL: the audio in binary frames */
    const char *append_type;
    const char *type_key;       /* name of the "type" field, sent and received ("message_type"), NULL: "type" */
    const char *audio_key;      /* name of the "audio" field ("audio_base_64"), NULL: "audio" */
    const char *append_extra;   /* fields after the audio (",\"commit\":false"), NULL for none */
    const char *commit_json;    /* the whole message ending the audio, instead of {"type": commit_type} */
    const char *clear_type;     /* message dropping the audio of an abandoned turn, NULL for none */
    const char *commit_type;    /* message ending the audio of the turn */
    /*
     * The server ends its session with the turn: connected when a turn starts (the audio is buffered meanwhile)
     * and closed after it, nothing open while the assistant sleeps
     */
    bool session_per_turn;
    /* A message received (type: its "type") */
    void (*on_message)(const cJSON *json, const char *type);
    /*
     * An HTTP service (Groq), NULL for a WebSocket one: the audio of the turn (16 kHz) is kept, then sent at once at
     * its end. Returns the text (allocated), NULL on failure (error: why); billed_s: the seconds billed.
     */
    char *(*transcribe)(const char *model, const char *language, const char *api_key, const int16_t *pcm,
                        size_t samples, double *billed_s, char *error, size_t error_size);
};

/** Text of the turn received so far (a delta, appended) */
void stt_backend_delta(const char *text);

/**
 * @brief The turn is transcribed.
 *
 * @param text       The whole transcript, or NULL for the deltas received
 * @param in_tokens  Billed tokens of audio heard (with out_tokens), or 0 to count its duration
 * @param out_tokens Billed tokens of text written
 * @param seconds    Duration of the audio billed, < 0 for the duration of the audio sent
 */
void stt_backend_final(const char *text, int in_tokens, int out_tokens, double seconds);

/** The end of the turn has been sent: the transcript received now is the final one */
bool stt_backend_awaiting(void);

/** The turn failed (code and message as the server gave them, for the log and the lack of credit) */
void stt_backend_failed(const char *code, const char *message);

#ifdef __cplusplus
}
#endif
