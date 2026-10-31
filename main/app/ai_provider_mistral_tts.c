/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Mistral AI speech (Voxtral TTS, voxtral-mini-tts-3-0): one streamed HTTP request per sentence,
 *
 *   POST https://api.mistral.ai/v1/audio/speech {model, input, voice_id, response_format: "wav", stream: true}
 *   -> server-sent events carrying "audio_data" (base64 audio, 24 kHz mono)
 *
 * WAV: 16-bit samples, half the bytes of "pcm" (32-bit floats): the stream comes faster than it is spoken. Its
 * header (first chunk) gives the sample format; without one, 32-bit floats and 16-bit integers are told apart on
 * the first audio. The events are read in blocks (an audio event is tens of KB: not byte by byte). The connection
 * is kept between sentences (keep-alive, tts_http.c). Billed by the character
 * ($13 a million): counted as a token a character. The voices and their emotion and pace tags come from [voices]
 * of AGENTS.INI; the tags ("[emotion=calm][pace=slow] ") are put before each sentence.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "llm_sse.h"
#include "tts_stream.h"
#include "tts_backend.h"
#include "tts_http.h"

static const char *TAG = "mistral_tts";

static const char *const s_headers[] = { "Accept", "text/event-stream", NULL };

static tts_http_t s_http = {
    .tag = "mistral_tts",
    .url = "https://api.mistral.ai/v1/audio/speech",
    .key_header = "Authorization",
    .key_format = "Bearer %s",
    .headers = s_headers,
};

typedef struct {
    llm_buf_t line;             /* line of the event stream being assembled */
    uint32_t generation;
    int format;                 /* 0: not known yet, 4: 32-bit float, 2: 16-bit integer */
    bool header_checked;        /* the WAV header of the first audio looked for */
    size_t received;            /* base64 audio received (bytes), for the log */
    uint8_t carry[4];           /* bytes of a sample cut between two audio chunks */
    size_t carry_len;
    int16_t *pcm16;             /* converted audio (PSRAM) */
    size_t pcm16_cap;
    uint8_t *raw;               /* decoded base64 (PSRAM) */
    size_t raw_cap;
    bool done;
    bool warm_up;               /* request of mistral_prewarm(): its audio is not played */
    bool logged_event;
    char error[160];
} stream_t;

static bool grow(void **buf, size_t *cap, size_t needed)
{
    if (needed <= *cap) {
        return true;
    }
    void *p = heap_caps_realloc(*buf, needed, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        return false;
    }
    *buf = p;
    *cap = needed;
    return true;
}

/* 32-bit float PCM if the first samples read as floats in [-2, 2] (16-bit samples read so are rarely all small) */
static int guess_format(const uint8_t *bytes, size_t len)
{
    size_t n = len / 4 < 64 ? len / 4 : 64;
    if (n == 0) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        float f;
        memcpy(&f, bytes + 4 * i, 4);
        if (isnan(f) || fabsf(f) > 2.0f) {
            return 2;
        }
    }
    return 4;
}

/* "RIFF....WAVEfmt ": its sample format (2: 16-bit integers, 4: 32-bit floats), and where its samples start */
static size_t wav_header(stream_t *st, const uint8_t *bytes, size_t len)
{
    if (len < 12 || memcmp(bytes, "RIFF", 4) != 0 || memcmp(bytes + 8, "WAVE", 4) != 0) {
        return 0;
    }
    size_t pos = 12;
    while (pos + 8 <= len) {
        uint32_t size = bytes[pos + 4] | bytes[pos + 5] << 8 | bytes[pos + 6] << 16 | (uint32_t)bytes[pos + 7] << 24;
        if (memcmp(bytes + pos, "fmt ", 4) == 0 && pos + 8 + 16 <= len) {
            int tag = bytes[pos + 8] | bytes[pos + 9] << 8;
            int bits = bytes[pos + 22] | bytes[pos + 23] << 8;
            st->format = (tag == 3 || bits == 32) ? 4 : 2;
        } else if (memcmp(bytes + pos, "data", 4) == 0) {
            return pos + 8;
        }
        pos += 8 + size;
    }
    return len;     // the header fills the chunk
}

/* Audio bytes of the stream (any cut) -> 16-bit samples for the speech engine */
static void on_pcm(stream_t *st, const uint8_t *bytes, size_t len)
{
    if (st->warm_up || st->generation != tts_backend_generation()) {
        return;
    }
    if (!st->header_checked) {
        st->header_checked = true;
        size_t skip = wav_header(st, bytes, len);
        bytes += skip;
        len -= skip;
        if (st->format) {
            ESP_LOGI(TAG, "audio: WAV, %s", st->format == 4 ? "32-bit float" : "16-bit");
        }
    }
    if (!st->format) {
        st->format = guess_format(bytes, len);
        if (!st->format) {
            return;
        }
        ESP_LOGI(TAG, "audio: %s PCM", st->format == 4 ? "32-bit float" : "16-bit");
    }
    if (st->format == 2) {
        tts_backend_audio(bytes, len);
        return;
    }
    if (!grow((void **)&st->pcm16, &st->pcm16_cap, (len / 4 + 2) * sizeof(int16_t))) {
        return;
    }
    size_t count = 0;
    for (size_t i = 0; i < len; i++) {
        st->carry[st->carry_len++] = bytes[i];
        if (st->carry_len == 4) {
            float f;
            memcpy(&f, st->carry, 4);
            st->carry_len = 0;
            f = f > 1.0f ? 1.0f : (f < -1.0f ? -1.0f : f);
            st->pcm16[count++] = (int16_t)lrintf(f * 32767.0f);
        }
    }
    tts_backend_audio((const uint8_t *)st->pcm16, count * sizeof(int16_t));
}

static void sse_dispatch(stream_t *st, const char *data)
{
    if (strcmp(data, "[DONE]") == 0) {
        st->done = true;
        return;
    }
    /* The audio fields are large: found without parsing the whole JSON */
    const char *audio = strstr(data, "\"audio_data\"");
    if (audio) {
        const char *start = strchr(audio + 12, '"');
        const char *end = start ? strchr(start + 1, '"') : NULL;
        if (start && end) {
            size_t b64_len = (size_t)(end - start - 1);
            size_t len = 0;
            st->received += b64_len;
            if (grow((void **)&st->raw, &st->raw_cap, b64_len / 4 * 3 + 4) &&
                    mbedtls_base64_decode(st->raw, st->raw_cap, &len, (const unsigned char *)start + 1, b64_len) == 0) {
                on_pcm(st, st->raw, len);
            } else {
                ESP_LOGE(TAG, "Bad audio chunk");
            }
        }
    }
    cJSON *json = audio ? NULL : cJSON_Parse(data);
    if (json) {
        const cJSON *type = cJSON_GetObjectItem(json, "type");
        const char *t = cJSON_IsString(type) ? type->valuestring : "";
        const cJSON *error = cJSON_GetObjectItem(json, "error");
        if (error || strstr(t, "error")) {
            const cJSON *message = cJSON_IsString(error) ? error : cJSON_GetObjectItem(error, "message");
            snprintf(st->error, sizeof(st->error), "%s", cJSON_IsString(message) ? message->valuestring : data);
            st->done = true;
        } else if (strstr(t, "done") || strstr(t, "completed")) {
            st->done = true;
        } else if (!st->logged_event) {
            st->logged_event = true;
            ESP_LOGI(TAG, "event: %.120s", data);     // an event of another kind, seen once (the format is new)
        }
        cJSON_Delete(json);
    }
}

/*
 * The event stream read in blocks: each "data: ..." line is one event (Mistral sends one line per event); a
 * line is only copied when a block cuts it
 */
static void feed(void *ctx, char *bytes, size_t len)
{
    stream_t *st = ctx;
    while (len > 0) {
        char *nl = memchr(bytes, '\n', len);
        size_t n = nl ? (size_t)(nl - bytes) : len;
        if (!nl) {
            llm_buf_append(&st->line, bytes, n);
            return;
        }
        char *line = bytes;
        size_t line_len = n;
        if (st->line.len) {
            llm_buf_append(&st->line, bytes, n);
            line = st->line.p;
            line_len = st->line.len;
        } else {
            *nl = '\0';        // the line as it is in the block
        }
        if (line_len && line[line_len - 1] == '\r') {
            line[--line_len] = '\0';
        }
        if (line_len > 5 && strncmp(line, "data:", 5) == 0) {
            sse_dispatch(st, line[5] == ' ' ? line + 6 : line + 5);
        }
        llm_buf_clear(&st->line);
        bytes = nl + 1;
        len -= n + 1;
    }
}

static char *build_request(const char *text)
{
    const tts_settings_t *settings = tts_backend_settings();
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", settings->model);
    // Emotion and pace tags of the voice ([voices] of AGENTS.INI) before the text: not spoken
    char *input = NULL;
    if (settings->style && settings->style[0]) {
        asprintf(&input, "%s%s", settings->style, text);
    }
    cJSON_AddStringToObject(req, "input", input ? input : text);
    free(input);
    cJSON_AddStringToObject(req, "voice_id", settings->voice ? settings->voice : "voice_589");
    cJSON_AddStringToObject(req, "response_format", "wav");
    cJSON_AddBoolToObject(req, "stream", true);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return body;
}

static void mistral_start(void)
{
}

static void mistral_stop(void)
{
    tts_http_stop(&s_http);
    ESP_LOGI(TAG, "Mistral speech closed");
}

static void mistral_upkeep(void)
{
}

/* A sentence, or with warm_up a request whose audio is dropped */
static void request(const char *text, uint32_t generation, bool warm_up)
{
    stream_t st = {
        .generation = generation,
        .warm_up = warm_up,
    };
    tts_http_reader_t reader = { .on_bytes = feed, .ctx = &st, .done = &st.done, .error = st.error };
    tts_http_request(&s_http, text, build_request(text), generation, warm_up, &reader);
    llm_buf_free(&st.line);
    free(st.pcm16);
    free(st.raw);
}

static void mistral_say(const char *text, uint32_t generation)
{
    request(text, generation, false);
}

/*
 * The user is speaking: a connection unused for 30 s is likely closed, and the first request of a new one takes
 * about 3.5 s instead of 0.7 (TLS handshake, then the service slower to answer). A "." is synthesized meanwhile,
 * not played (a character: next to nothing).
 */
static void warm_up(void)
{
    request(".", 0, true);
}

static void mistral_prewarm(void)
{
    tts_http_prewarm(&s_http, warm_up);
}

const tts_backend_t tts_backend_mistral = {
    .name = "mistral",
    .start = mistral_start,
    .stop = mistral_stop,
    .upkeep = mistral_upkeep,
    .say = mistral_say,
    .prewarm = mistral_prewarm,
    // A request takes 0.7 to 1 s to start, and its audio comes about 1.15 times faster than spoken: the sentences
    // waiting go in one request (no wait between them), and more audio is kept in reserve before playing
    .join_chars = 600,
    .reserve_ms = 600,
};
