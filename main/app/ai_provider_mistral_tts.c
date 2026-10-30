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
 * is kept between sentences (keep-alive). Billed by the character
 * ($13 a million): counted as a token a character. The voices and their emotion and pace tags come from [voices]
 * of AGENTS.INI; the tags ("[emotion=calm][pace=slow] ") are put before each sentence.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "file_costs.h"
#include "llm_sse.h"
#include "tts_stream.h"
#include "tts_backend.h"

#define MISTRAL_TTS_URL         "https://api.mistral.ai/v1/audio/speech"
#define MISTRAL_TTS_TIMEOUT_MS  15000
#define MISTRAL_TTS_READ_CHUNK  4096

static const char *TAG = "mistral_tts";

static esp_http_client_handle_t s_client = NULL;
static char *s_auth = NULL;

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

static void drop_client(void)
{
    if (s_client) {
        esp_http_client_close(s_client);
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
}

static esp_http_client_handle_t get_client(void)
{
    if (!s_client) {
        esp_http_client_config_t config = {
            .url = MISTRAL_TTS_URL,
            .method = HTTP_METHOD_POST,
            .timeout_ms = MISTRAL_TTS_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = true,
        };
        s_client = esp_http_client_init(&config);
        if (s_client) {
            esp_http_client_set_header(s_client, "Content-Type", "application/json");
            esp_http_client_set_header(s_client, "Accept", "text/event-stream");
        }
    }
    if (s_client) {
        free(s_auth);
        s_auth = NULL;
        asprintf(&s_auth, "Bearer %s", tts_backend_settings()->api_key ? tts_backend_settings()->api_key : "");
        esp_http_client_set_header(s_client, "Authorization", s_auth);
    }
    return s_client;
}

/*
 * The event stream read in blocks: each "data: ..." line is one event (Mistral sends one line per event); a
 * line is only copied when a block cuts it
 */
static void feed(stream_t *st, char *bytes, size_t len)
{
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
    drop_client();
    ESP_LOGI(TAG, "Mistral speech closed");
}

static void mistral_upkeep(void)
{
}

/* Last request answered (esp_timer us): a connection unused for long is likely closed by the server */
static int64_t s_last_used_us = 0;
#define MISTRAL_TTS_WARM_US     (30 * 1000000LL)

/* A sentence, or with warm_up a request whose audio is dropped (nothing reported to the speech engine) */
static void request(const char *text, uint32_t generation, bool warm_up)
{
    if (!warm_up) {
        tts_backend_sentence_start(generation);
    }
    stream_t st = {
        .generation = generation,
        .warm_up = warm_up,
    };
    char *body = build_request(text);
    int64_t start_us = esp_timer_get_time();
    const char *status_text = "completed";
    bool failed = false;
    int status = -1;
    for (int attempt = 0; body && attempt < 2; attempt++) {
        esp_http_client_handle_t client = get_client();
        size_t len = strlen(body);
        if (client && esp_http_client_open(client, len) == ESP_OK && esp_http_client_write(client, body, len) == (int)len &&
                esp_http_client_fetch_headers(client) >= 0) {
            status = esp_http_client_get_status_code(client);
            break;
        }
        drop_client();      // a kept-alive connection gone stale: once more on a fresh one
    }
    static char *chunk = NULL;     // the block read (PSRAM)
    if (!chunk) {
        chunk = heap_caps_malloc(MISTRAL_TTS_READ_CHUNK + 1, MALLOC_CAP_SPIRAM);
    }
    int n;
    if (!chunk) {
        status_text = "no memory";
        failed = true;
    } else if (status != 200) {
        llm_buf_t err = {0};
        while (s_client && status > 0 && (n = esp_http_client_read(s_client, chunk, MISTRAL_TTS_READ_CHUNK)) > 0) {
            llm_buf_append(&err, chunk, n);
        }
        ESP_LOGE(TAG, "HTTP %d: %.300s", status, err.len ? err.p : "(no answer)");
        if (status == 402 || tts_backend_is_no_credit(err.p)) {
            tts_backend_no_credit();
        }
        llm_buf_free(&err);
        drop_client();
        status_text = "failed";
        failed = (status <= 0);
    } else {
        while (!st.done && (n = esp_http_client_read(s_client, chunk, MISTRAL_TTS_READ_CHUNK)) > 0) {
            if (!warm_up && generation != tts_backend_generation()) {
                status_text = "cancelled";
                break;
            }
            feed(&st, chunk, n);
        }
        int64_t ms = (esp_timer_get_time() - start_us) / 1000;
        ESP_LOGI(TAG, "%s%u KB of audio received in %lld ms", warm_up ? "warm-up: " : "",
                 (unsigned)(st.received / 1024), ms);
        if (st.error[0]) {
            ESP_LOGE(TAG, "error: %s", st.error);
            if (tts_backend_is_no_credit(st.error)) {
                tts_backend_no_credit();
            }
            status_text = "failed";
        } else if (!st.done && strcmp(status_text, "cancelled") != 0 && !esp_http_client_is_complete_data_received(s_client)) {
            status_text = "cut";
            failed = true;
        }
        if (strcmp(status_text, "completed") == 0) {
            while (esp_http_client_read(s_client, chunk, MISTRAL_TTS_READ_CHUNK) > 0) {
            }
        }
        if (strcmp(status_text, "completed") != 0 || !esp_http_client_is_complete_data_received(s_client)) {
            drop_client();
        }
        // Billed by the character: a token a character
        costs_add_tokens(COST_TTS, tts_backend_settings()->model, (int)strlen(text), 0, 0, 0);
        s_last_used_us = esp_timer_get_time();
    }
    if (!warm_up) {
        tts_backend_sentence_end(status_text, failed);
    }
    llm_buf_free(&st.line);
    free(st.pcm16);
    free(st.raw);
    free(body);
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
static void mistral_prewarm(void)
{
    if (s_client && esp_timer_get_time() - s_last_used_us < MISTRAL_TTS_WARM_US) {
        return;
    }
    int64_t start_us = esp_timer_get_time();
    request(".", 0, true);
    ESP_LOGI(TAG, "warmed up in %lld ms", (esp_timer_get_time() - start_us) / 1000);
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
