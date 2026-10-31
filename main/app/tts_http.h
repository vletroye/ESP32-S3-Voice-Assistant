/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Speech services asked by one HTTP request per sentence (Deepgram, ElevenLabs, Mistral, OpenRouter), shared by
 * their ai_provider_<name>_tts.c: the connection kept between sentences (keep-alive, a stale one retried), the
 * error of a refused request (402: no credit), the audio read in blocks as it comes and given to the service's
 * decoder, a reply cancelled or cut, the cost, and the warm-up while the user speaks.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A service's connection: static, its first fields set by the service, the rest kept by tts_http.c */
typedef struct {
    const char *tag;                /* of the logs */
    const char *url;                /* the URL, or NULL: url_of() */
    char *(*url_of)(void);          /* made for each request (the voice in it), allocated */
    const char *key_header;         /* "Authorization", "xi-api-key" */
    const char *key_format;         /* its value: "Bearer %s", "Token %s", "%s" */
    const char *const *headers;     /* more headers: name, value, ..., NULL; NULL for none */
    unsigned frame_bytes;           /* the audio given in whole multiples of this (2: 16-bit samples), 0: as it comes */

    esp_http_client_handle_t client;
    char *url_made;
    char *key_value;
    int64_t last_used_us;           /* last request answered */
} tts_http_t;

/* What reads the answer of a request */
typedef struct {
    /* Audio bytes received (len up to 4 KB, writable): decoded and given to the speech engine (a warm-up: dropped) */
    void (*on_bytes)(void *ctx, char *bytes, size_t len);
    void *ctx;
    const bool *done;               /* NULL, or set by on_bytes when the stream says it is over ([DONE]) */
    const char *error;              /* NULL, or an error said in the stream, "" if none */
} tts_http_reader_t;

/**
 * @brief A sentence spoken (or with warm_up, a request whose audio is dropped: nothing reported to the speech
 *        engine): the request sent with body (freed here), its answer read into reader.
 */
void tts_http_request(tts_http_t *h, const char *text, char *body, uint32_t generation, bool warm_up,
                      const tts_http_reader_t *reader);

/**
 * @brief The user is speaking: a connection unused for a while is likely closed by the server, opened again
 *        (TLS handshake) by warm_up(), which says a "." not played (a character: next to nothing).
 */
void tts_http_prewarm(tts_http_t *h, void (*warm_up)(void));

/** Another service chosen: the connection closed */
void tts_http_stop(tts_http_t *h);

#ifdef __cplusplus
}
#endif
