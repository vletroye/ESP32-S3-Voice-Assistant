/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Sentence-by-sentence speech synthesis with OpenAI /v1/audio/speech (raw PCM, chunked transfer).
 *
 *   tts_stream_say() -> sentence queue -> [tts task] HTTP stream, 24 kHz -> 16 kHz
 *                    -> audio stream buffer (PSRAM) -> [playback task] I2S (16 kHz stereo)
 *
 * The I2S bus stays at 16 kHz, shared with the microphones, so that listening is never interrupted.
 */

#include <math.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "bsp_board.h"
#include "tts_stream.h"

#define TTS_SAMPLE_RATE_IN      24000   /* OpenAI "pcm": 24 kHz, 16-bit signed, mono, little-endian */
#define TTS_AUDIO_BUFFER_BYTES  (256 * 1024)    /* 8 s of 16 kHz mono audio, in PSRAM */
#define TTS_SENTENCE_QUEUE_LEN  16
#define TTS_HTTP_TIMEOUT_MS     15000
#define TTS_READ_CHUNK          1536    /* multiple of 6 bytes: 3 input samples -> 2 output samples */
#define PLAYBACK_FRAMES         256

#define RESAMPLER_TAPS          15

static const char *TAG = "tts";

typedef struct {
    char *text;             /* NULL marks the end of the reply */
    uint32_t generation;
    bool prewarm;           /* open the connection only */
} sentence_item_t;

static QueueHandle_t s_sentences = NULL;
static StreamBufferHandle_t s_audio = NULL;

/* Incremented for each new reply and on cancel: work tagged with an older value is dropped */
static volatile uint32_t s_generation = 0;
/* Generation whose last sentence has been synthesized */
static volatile uint32_t s_synth_done_generation = 0;
/* Set on cancel: the playback task drops the buffered audio */
static volatile bool s_flush = false;
static tts_event_cb_t s_event_cb = NULL;

static char *s_url = NULL;
static char *s_prewarm_url = NULL;
static char *s_auth = NULL;
static char *s_model = NULL;
static char *s_voice = NULL;
static char *s_instructions = NULL;
static esp_http_client_handle_t s_client = NULL;

/* ---------- 24 kHz -> 16 kHz resampler ---------- */

/* Low-pass FIR at 7 kHz (below the 8 kHz Nyquist of the output), Q15 */
static int16_t s_fir[RESAMPLER_TAPS];

typedef struct {
    int16_t history[RESAMPLER_TAPS];
    int pos;            /* index of the next input sample, modulo 3 */
    int32_t pending;    /* filtered sample kept for the averaged output */
    bool has_odd_byte;
    uint8_t odd_byte;
} resampler_t;

static void resampler_design(void)
{
    const float fc = 7000.0f / TTS_SAMPLE_RATE_IN;
    const int mid = RESAMPLER_TAPS / 2;
    float coef[RESAMPLER_TAPS];
    float sum = 0;
    for (int i = 0; i < RESAMPLER_TAPS; i++) {
        int n = i - mid;
        float sinc = (n == 0) ? 2.0f * fc : sinf(2.0f * (float)M_PI * fc * n) / ((float)M_PI * n);
        float window = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * i / (RESAMPLER_TAPS - 1));
        coef[i] = sinc * window;
        sum += coef[i];
    }
    for (int i = 0; i < RESAMPLER_TAPS; i++) {
        s_fir[i] = (int16_t)lrintf(coef[i] / sum * 32767.0f);
    }
}

static int16_t clamp16(int32_t v)
{
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v);
}

/*
 * Filter and decimate by 3/2: of every 3 filtered input samples y0 y1 y2, output y0 and (y1 + y2) / 2
 * (the sample half-way between them). Returns the number of output samples written to out.
 */
static size_t resample(resampler_t *r, const uint8_t *bytes, size_t len, int16_t *out)
{
    size_t n_out = 0;
    size_t i = 0;
    while (i < len) {
        int16_t x;
        if (r->has_odd_byte) {
            x = (int16_t)(r->odd_byte | (bytes[i] << 8));
            r->has_odd_byte = false;
            i += 1;
        } else if (i + 1 < len) {
            x = (int16_t)(bytes[i] | (bytes[i + 1] << 8));
            i += 2;
        } else {
            r->odd_byte = bytes[i];
            r->has_odd_byte = true;
            break;
        }
        memmove(r->history, r->history + 1, (RESAMPLER_TAPS - 1) * sizeof(int16_t));
        r->history[RESAMPLER_TAPS - 1] = x;
        int32_t acc = 0;
        for (int k = 0; k < RESAMPLER_TAPS; k++) {
            acc += (int32_t)s_fir[k] * r->history[k];
        }
        int32_t y = acc >> 15;
        switch (r->pos) {
        case 0:
            out[n_out++] = clamp16(y);
            break;
        case 1:
            r->pending = y;
            break;
        default:
            out[n_out++] = clamp16((r->pending + y) / 2);
            break;
        }
        r->pos = (r->pos + 1) % 3;
    }
    return n_out;
}

/* ---------- synthesis ---------- */

static void replace_str(char **dst, const char *src)
{
    free(*dst);
    *dst = (src && src[0]) ? strdup(src) : NULL;
}

void tts_stream_configure(const char *base_url, const char *api_key, const char *model, const char *voice,
                          const char *instructions)
{
    char *url = NULL;
    char *prewarm_url = NULL;
    char *auth = NULL;
    asprintf(&url, "%saudio/speech", base_url);
    asprintf(&prewarm_url, "%smodels/%s", base_url, model);
    asprintf(&auth, "Bearer %s", api_key);
    free(s_url);
    free(s_prewarm_url);
    free(s_auth);
    s_url = url;
    s_prewarm_url = prewarm_url;
    s_auth = auth;
    replace_str(&s_model, model);
    replace_str(&s_voice, voice);
    replace_str(&s_instructions, instructions);
    ESP_LOGI(TAG, "model: %s, voice: %s", s_model ? s_model : "-", s_voice ? s_voice : "-");
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
            .url = s_url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = TTS_HTTP_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = true,
        };
        s_client = esp_http_client_init(&config);
        if (s_client) {
            esp_http_client_set_header(s_client, "Content-Type", "application/json");
            esp_http_client_set_header(s_client, "Authorization", s_auth);
        }
    }
    return s_client;
}

static char *build_request(const char *text)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", s_model);
    cJSON_AddStringToObject(req, "voice", s_voice);
    cJSON_AddStringToObject(req, "input", text);
    cJSON_AddStringToObject(req, "response_format", "pcm");
    if (s_instructions) {
        cJSON_AddStringToObject(req, "instructions", s_instructions);
    }
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return body;
}

/* Send audio to the playback buffer, giving up if the reply was cancelled meanwhile */
static bool push_audio(const int16_t *samples, size_t count, uint32_t generation)
{
    const uint8_t *p = (const uint8_t *)samples;
    size_t left = count * sizeof(int16_t);
    while (left > 0) {
        if (generation != s_generation) {
            return false;
        }
        size_t sent = xStreamBufferSend(s_audio, p, left, pdMS_TO_TICKS(100));
        p += sent;
        left -= sent;
    }
    return true;
}

static void synthesize(const char *text, uint32_t generation)
{
    if (!s_url || !s_model || !s_voice) {
        ESP_LOGE(TAG, "Not configured");
        return;
    }
    char *body = build_request(text);
    if (!body) {
        return;
    }
    int64_t t0 = esp_timer_get_time();
    int64_t t_first = 0;
    size_t len = strlen(body);
    int status = 0;
    esp_http_client_handle_t client = NULL;

    /* A kept-alive connection may have been closed by the server: retry once on a new one */
    for (int attempt = 0; attempt < 2 && !client; attempt++) {
        client = get_client();
        if (client && esp_http_client_open(client, len) == ESP_OK &&
                esp_http_client_write(client, body, len) == (int)len &&
                esp_http_client_fetch_headers(client) >= 0) {
            status = esp_http_client_get_status_code(client);
        } else {
            drop_client();
            client = NULL;
        }
    }
    free(body);
    if (!client) {
        ESP_LOGE(TAG, "Request failed");
        return;
    }

    static uint8_t chunk[TTS_READ_CHUNK];
    static int16_t samples[TTS_READ_CHUNK / 3 + 2];
    int n;
    if (status != 200) {
        n = esp_http_client_read(client, (char *)chunk, sizeof(chunk) - 1);
        chunk[n > 0 ? n : 0] = '\0';
        ESP_LOGE(TAG, "HTTP %d: %.300s", status, (char *)chunk);
        drop_client();
        return;
    }

    resampler_t resampler = {0};
    size_t total = 0;
    bool cancelled = false;
    while ((n = esp_http_client_read(client, (char *)chunk, sizeof(chunk))) > 0) {
        if (!t_first) {
            t_first = esp_timer_get_time();
        }
        total += n;
        size_t count = resample(&resampler, chunk, n, samples);
        if (!push_audio(samples, count, generation)) {
            cancelled = true;
            break;
        }
    }
    if (cancelled || n < 0 || !esp_http_client_is_complete_data_received(client)) {
        drop_client();
    }
    ESP_LOGI(TAG, "\"%.40s%s\": first audio %lld ms, total %lld ms, %u bytes%s", text, strlen(text) > 40 ? "..." : "",
             t_first ? (t_first - t0) / 1000 : -1LL, (esp_timer_get_time() - t0) / 1000, (unsigned)total,
             cancelled ? " (cancelled)" : "");
}

/* Open the TLS connection in advance with a free, tiny request (the model description) */
static void prewarm_connection(void)
{
    if (!s_url || !s_prewarm_url) {
        return;
    }
    int64_t t0 = esp_timer_get_time();
    esp_http_client_handle_t client = get_client();
    if (!client) {
        return;
    }
    esp_http_client_set_url(client, s_prewarm_url);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    int status = -1;
    if (esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
        status = esp_http_client_get_status_code(client);
        /*
         * Read the body to its end: esp_http_client_flush_response() can leave a small body that came
         * with the headers in the connection, where it would be read as the start of the next response
         */
        char discard[256];
        while (esp_http_client_read(client, discard, sizeof(discard)) > 0) {
        }
    }
    if (status == 200 && esp_http_client_is_complete_data_received(client)) {
        esp_http_client_set_url(client, s_url);
        esp_http_client_set_method(client, HTTP_METHOD_POST);
    } else {
        drop_client();
    }
    ESP_LOGI(TAG, "prewarm: HTTP %d in %lld ms", status, (esp_timer_get_time() - t0) / 1000);
}

static void tts_task(void *arg)
{
    sentence_item_t item;
    for (;;) {
        if (xQueueReceive(s_sentences, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (item.prewarm) {
            prewarm_connection();
        } else if (item.generation == s_generation) {
            if (item.text) {
                synthesize(item.text, item.generation);
            } else {
                s_synth_done_generation = item.generation;
            }
        }
        free(item.text);
    }
}

/* ---------- playback ---------- */

/*
 * Audio kept in reserve before playing: the first bytes of a sentence arrive in small irregular bursts,
 * and playing them as they come starves the I2S output (crackles). Once a sentence is flowing, the
 * synthesis runs faster than real time.
 */
#define PREBUFFER_START_BYTES   (200 * 16 * sizeof(int16_t))    /* 200 ms at 16 kHz mono */
#define PREBUFFER_RESUME_BYTES  (100 * 16 * sizeof(int16_t))    /* 100 ms after running dry */

static void playback_task(void *arg)
{
    static int16_t mono[PLAYBACK_FRAMES];
    static int16_t stereo[PLAYBACK_FRAMES * 2];
    uint32_t playing_generation = 0;
    bool started = false;       /* first audio of the reply played */
    bool playing = false;       /* feeding the I2S output (else filling the reserve) */
    bool done_sent = true;
    unsigned underruns = 0;

    for (;;) {
        if (s_flush) {
            while (xStreamBufferReceive(s_audio, mono, sizeof(mono), 0) > 0) {
            }
            s_flush = false;
        }
        uint32_t generation = s_generation;
        if (generation != playing_generation) {
            playing_generation = generation;
            started = false;
            playing = false;
            done_sent = false;
            underruns = 0;
        }
        bool synth_done = (s_synth_done_generation == playing_generation);
        size_t available = xStreamBufferBytesAvailable(s_audio);

        if (!playing) {
            size_t needed = started ? PREBUFFER_RESUME_BYTES : PREBUFFER_START_BYTES;
            if (available >= needed || (synth_done && available > 0)) {
                playing = true;
            } else {
                if (synth_done && available == 0 && !done_sent) {
                    done_sent = true;
                    ESP_LOGI(TAG, "reply played, %u underrun(s)", underruns);
                    if (s_event_cb) {
                        s_event_cb(TTS_EVENT_DONE);
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
        }

        size_t n = xStreamBufferReceive(s_audio, mono, sizeof(mono), pdMS_TO_TICKS(20));
        if (n >= sizeof(int16_t)) {
            if (!started) {
                started = true;
                if (s_event_cb) {
                    s_event_cb(TTS_EVENT_FIRST_AUDIO);
                }
            }
            size_t frames = n / sizeof(int16_t);
            for (size_t i = 0; i < frames; i++) {
                stereo[2 * i] = mono[i];
                stereo[2 * i + 1] = mono[i];
            }
            size_t written = 0;
            bsp_i2s_write(stereo, frames * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        } else {
            /* Ran dry: wait for the end of the reply, or refill the reserve before resuming */
            playing = false;
            if (!synth_done) {
                underruns++;
            }
        }
    }
}

/* ---------- public API ---------- */

esp_err_t tts_stream_init(void)
{
    ESP_RETURN_ON_FALSE(!s_sentences, ESP_ERR_INVALID_STATE, TAG, "Already initialized");
    resampler_design();
    s_sentences = xQueueCreate(TTS_SENTENCE_QUEUE_LEN, sizeof(sentence_item_t));
    s_audio = xStreamBufferCreateWithCaps(TTS_AUDIO_BUFFER_BYTES, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_sentences && s_audio, ESP_ERR_NO_MEM, TAG, "Failed to create queues");

    /* Stacks in PSRAM: internal RAM is too fragmented for 8 KB blocks once Wi-Fi and the AFE run */
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(tts_task, "tts", 8 * 1024, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
    ok &= xTaskCreatePinnedToCoreWithCaps(playback_task, "tts_play", 4 * 1024, NULL, 6, NULL, 1, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "Failed to create tasks");
    return ESP_OK;
}

static void drop_queued_sentences(void)
{
    sentence_item_t item;
    while (xQueueReceive(s_sentences, &item, 0) == pdTRUE) {
        free(item.text);
    }
}

void tts_stream_begin(tts_event_cb_t cb)
{
    s_generation++;
    drop_queued_sentences();
    s_flush = true;
    s_event_cb = cb;
}

void tts_stream_say(const char *sentence)
{
    size_t len = strlen(sentence) + 1;
    sentence_item_t item = {
        .text = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .generation = s_generation,
    };
    if (item.text) {
        memcpy(item.text, sentence, len);
    }
    if (item.text && xQueueSend(s_sentences, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Sentence queue full, dropped: %.40s", sentence);
        free(item.text);
    }
}

void tts_stream_end(void)
{
    sentence_item_t item = {
        .text = NULL,
        .generation = s_generation,
    };
    xQueueSend(s_sentences, &item, portMAX_DELAY);
}

void tts_stream_prewarm(void)
{
    sentence_item_t item = {
        .prewarm = true,
    };
    xQueueSend(s_sentences, &item, 0);
}

void tts_stream_cancel(void)
{
    s_generation++;
    drop_queued_sentences();
    s_flush = true;
}
