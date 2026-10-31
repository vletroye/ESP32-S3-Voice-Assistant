/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Groq transcription (Whisper large v3 / v3 turbo): Groq has no streaming transcription, so the audio of the question
 * is kept by stt_stream.c, then sent in one request once the user has stopped speaking:
 *
 *   POST https://api.groq.com/openai/v1/audio/transcriptions (header "Authorization: Bearer <key>")
 *   multipart/form-data: file (WAV, 16 kHz mono 16-bit), model, language, response_format=json, temperature=0
 *   -> {"text": "..."}
 *
 * Billed by the hour of audio, at least 10 seconds a request.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "llm_sse.h"
#include "stt_backend.h"
#include "stt_stream.h"

#define GROQ_STT_URL            "https://api.groq.com/openai/v1/audio/transcriptions"
#define GROQ_STT_TIMEOUT_MS     15000
#define GROQ_STT_BOUNDARY       "----esp32s3assistant"
#define GROQ_STT_MIN_BILLED_S   10
#define GROQ_STT_RATE           16000

static const char *TAG = "groq_stt";

/* "--boundary\r\nContent-Disposition: form-data; name="model"\r\n\r\nwhisper-large-v3-turbo\r\n" (allocated) */
static char *form_field(const char *name, const char *value)
{
    char *part = NULL;
    asprintf(&part, "--" GROQ_STT_BOUNDARY "\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n", name,
             value);
    return part;
}

/* The 44 bytes of a WAV header: 16 kHz, mono, 16-bit */
static void wav_header(uint8_t h[44], size_t samples)
{
    uint32_t data = (uint32_t)(samples * 2);
    uint32_t rate = GROQ_STT_RATE, byte_rate = GROQ_STT_RATE * 2, riff = 36 + data, fmt_size = 16;
    uint16_t format = 1, channels = 1, align = 2, bits = 16;
    memcpy(h, "RIFF", 4);
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    memcpy(h + 16, &fmt_size, 4);
    memcpy(h + 20, &format, 2);
    memcpy(h + 22, &channels, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
}

static char *groq_transcribe(const char *model, const char *language, const char *api_key, const int16_t *pcm,
                             size_t samples, double *billed_s, char *error, size_t error_size)
{
    int64_t t0 = esp_timer_get_time();
    char *fields = NULL;
    char *f_model = form_field("model", model);
    char *f_format = form_field("response_format", "json");
    char *f_temperature = form_field("temperature", "0");
    // The language in use ("fr"): faster and more accurate than guessed
    char *f_language = (language && strlen(language) == 2) ? form_field("language", language) : strdup("");
    asprintf(&fields, "%s%s%s%s", f_model ? f_model : "", f_format ? f_format : "", f_temperature ? f_temperature : "",
             f_language ? f_language : "");
    free(f_model);
    free(f_format);
    free(f_temperature);
    free(f_language);
    static const char file_head[] = "--" GROQ_STT_BOUNDARY "\r\nContent-Disposition: form-data; name=\"file\"; "
                                    "filename=\"question.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    static const char tail[] = "\r\n--" GROQ_STT_BOUNDARY "--\r\n";
    uint8_t wav[44];
    wav_header(wav, samples);
    size_t audio_len = samples * sizeof(int16_t);
    size_t total = (fields ? strlen(fields) : 0) + sizeof(file_head) - 1 + sizeof(wav) + audio_len + sizeof(tail) - 1;

    esp_http_client_config_t config = {
        .url = GROQ_STT_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = GROQ_STT_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = fields ? esp_http_client_init(&config) : NULL;
    char *auth = NULL;
    asprintf(&auth, "Bearer %s", api_key);
    char *text = NULL;
    int status = -1;
    llm_buf_t body = {0};
    if (client && auth) {
        esp_http_client_set_header(client, "Authorization", auth);
        esp_http_client_set_header(client, "Content-Type", "multipart/form-data; boundary=" GROQ_STT_BOUNDARY);
        bool sent = esp_http_client_open(client, total) == ESP_OK &&
                    esp_http_client_write(client, fields, strlen(fields)) == (int)strlen(fields) &&
                    esp_http_client_write(client, file_head, sizeof(file_head) - 1) == (int)(sizeof(file_head) - 1) &&
                    esp_http_client_write(client, (const char *)wav, sizeof(wav)) == (int)sizeof(wav);
        // The audio in blocks (a long question is hundreds of KB)
        for (size_t pos = 0; sent && pos < audio_len; pos += 8192) {
            int n = (int)(audio_len - pos < 8192 ? audio_len - pos : 8192);
            sent = esp_http_client_write(client, (const char *)pcm + pos, n) == n;
        }
        sent = sent && esp_http_client_write(client, tail, sizeof(tail) - 1) == (int)(sizeof(tail) - 1);
        if (sent && esp_http_client_fetch_headers(client) >= 0) {
            status = esp_http_client_get_status_code(client);
            char chunk[512];
            int n;
            while ((n = esp_http_client_read(client, chunk, sizeof(chunk))) > 0) {
                llm_buf_append(&body, chunk, n);
            }
        }
    }
    // {"text": "..."} or {"error": {"message": "...", "type": "...", "code": "..."}}
    cJSON *json = body.len ? cJSON_Parse(body.p) : NULL;
    const cJSON *t = cJSON_GetObjectItem(json, "text");
    if (status == 200 && cJSON_IsString(t)) {
        // Whisper starts its text with a space
        const char *s = t->valuestring;
        while (*s == ' ') {
            s++;
        }
        text = strdup(s);
    } else {
        const cJSON *message = cJSON_GetObjectItem(cJSON_GetObjectItem(json, "error"), "message");
        snprintf(error, error_size, "HTTP %d: %s", status,
                 cJSON_IsString(message) ? message->valuestring : (body.len ? body.p : "no answer"));
    }
    double seconds = (double)samples / GROQ_STT_RATE;
    *billed_s = seconds < GROQ_STT_MIN_BILLED_S ? GROQ_STT_MIN_BILLED_S : seconds;
    ESP_LOGI(TAG, "%u KB sent, HTTP %d in %lld ms", (unsigned)(total / 1024), status,
             (esp_timer_get_time() - t0) / 1000);
    cJSON_Delete(json);
    llm_buf_free(&body);
    if (client) {
        esp_http_client_cleanup(client);
    }
    free(auth);
    free(fields);
    return text;
}

static char *groq_uri(const char *model, const char *language)
{
    return strdup(GROQ_STT_URL);
}

const stt_backend_t stt_backend_groq = {
    .name = "groq",
    .rate = 16000,
    .uri = groq_uri,
    .session_per_turn = false,
    .transcribe = groq_transcribe,
};
