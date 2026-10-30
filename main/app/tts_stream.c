/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Sentence-by-sentence speech synthesis, by the service chosen ([tts] of AGENTS.INI: ai_provider_openai_tts.c,
 * ai_provider_mistral_tts.c).
 *
 *   tts_stream_say() -> sentence queue -> [tts task] the service speaks the sentence -> 24 kHz audio
 *   -> 16 kHz -> audio stream buffer (PSRAM) -> [playback task] I2S (16 kHz stereo)
 *
 * The I2S bus stays at 16 kHz, shared with the microphones, so that listening is never interrupted.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "bsp_board.h"
#include "radio.h"
#include "app_wifi.h"
#include "tts_stream.h"
#include "tts_backend.h"

#define TTS_SAMPLE_RATE_IN      24000   /* audio given by the services: 24 kHz, 16-bit signed, mono */
#define TTS_AUDIO_BUFFER_BYTES  (256 * 1024)    /* 8 s of 16 kHz mono audio, in PSRAM */
#define TTS_SENTENCE_QUEUE_LEN  16
/* Cuts in the middle of a sentence from which the reply is reported as choppy */
#define TTS_CHOPPY_CUTS             2
#define PLAYBACK_FRAMES         256

#define RESAMPLER_TAPS          15

static const char *TAG = "tts";

typedef struct {
    char *text;             /* NULL marks the end of the reply */
    uint32_t generation;
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
/* Last audio sent to the speaker (tts_stream_is_silent) */
static volatile int64_t s_last_play_us = 0;

/* The service and what it uses; a service chosen by tts_stream_configure(), taken by the synthesis task */
static const tts_backend_t *s_backend = NULL;
static const tts_backend_t *volatile s_next_backend = NULL;
static volatile bool s_configuring = false;     /* settings being changed: not applied before they all are */
static tts_settings_t s_settings;
static SemaphoreHandle_t s_settings_lock = NULL;

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

/* ---------- the sentence being spoken (given by the service) ---------- */

static resampler_t s_resampler;
static volatile bool s_sentence_active = false;
static volatile bool s_sentence_has_audio = false;
static volatile uint32_t s_sentence_generation = 0;
static int64_t s_sentence_us = 0;
static int64_t s_first_audio_us = 0;
static size_t s_audio_bytes = 0;
static const char *s_sentence_text = "";
/* Quality of the current reply: speech run dry in the middle of a sentence, sentences lost */
static volatile unsigned s_cuts = 0;
static volatile bool s_failed = false;
/* The speech refused for lack of credit: said on screen after the reply */
static volatile bool s_no_credit = false;
static int16_t *s_resampled = NULL;
static size_t s_resampled_cap = 0;
/* Model test: the audio of the sentences kept here (16 kHz, PSRAM) instead of being played */
static volatile bool s_capturing = false;
static int16_t *s_capture = NULL;
static size_t s_capture_len = 0;    /* samples */
static size_t s_capture_cap = 0;
static int s_test_first_ms = -1;
static int s_test_total_ms = -1;

static void capture(const int16_t *samples, size_t count)
{
    if (s_capture_len + count > s_capture_cap) {
        size_t cap = s_capture_cap ? s_capture_cap * 2 : 16000 * 4;
        while (cap < s_capture_len + count) {
            cap *= 2;
        }
        int16_t *p = heap_caps_realloc(s_capture, cap * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!p) {
            return;
        }
        s_capture = p;
        s_capture_cap = cap;
    }
    memcpy(s_capture + s_capture_len, samples, count * sizeof(int16_t));
    s_capture_len += count;
}

/* Last sample of the sentence: brought down to 0 at its end (a service that stops on a sample far from 0 clicks) */
static int16_t s_last_sample = 0;
#define FADE_OUT_SAMPLES    128     /* 8 ms at 16 kHz */

static void replace_str(char **dst, const char *src)
{
    free(*dst);
    *dst = (src && src[0]) ? strdup(src) : NULL;
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

const tts_settings_t *tts_backend_settings(void)
{
    return &s_settings;
}

uint32_t tts_backend_generation(void)
{
    return s_generation;
}

void tts_backend_sentence_start(uint32_t generation)
{
    s_resampler = (resampler_t) {0};
    s_first_audio_us = 0;
    s_audio_bytes = 0;
    s_sentence_generation = generation;
    s_sentence_has_audio = false;
    s_last_sample = 0;
    s_sentence_us = esp_timer_get_time();
    s_sentence_active = true;
}

void tts_backend_audio(const uint8_t *pcm, size_t len)
{
    uint32_t generation = s_sentence_generation;
    if (!s_sentence_active || generation != s_generation || len == 0) {
        return;
    }
    if (!s_first_audio_us) {
        s_first_audio_us = esp_timer_get_time();
    }
    s_sentence_has_audio = true;
    s_audio_bytes += len;
    size_t needed = (len / 3 + 2) * sizeof(int16_t);
    if (needed > s_resampled_cap) {
        int16_t *p = heap_caps_realloc(s_resampled, needed, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) {
            return;
        }
        s_resampled = p;
        s_resampled_cap = needed;
    }
    size_t count = resample(&s_resampler, pcm, len, s_resampled);
    if (count) {
        s_last_sample = s_resampled[count - 1];
    }
    if (s_capturing) {
        capture(s_resampled, count);
        return;
    }
    push_audio(s_resampled, count, generation);
}

void tts_backend_sentence_end(const char *status, bool failed)
{
    s_test_first_ms = s_first_audio_us ? (int)((s_first_audio_us - s_sentence_us) / 1000) : -1;
    s_test_total_ms = (int)((esp_timer_get_time() - s_sentence_us) / 1000);
    if (s_sentence_has_audio && s_last_sample && s_sentence_generation == s_generation && !s_capturing) {
        // A short ramp from the last sample to silence
        int16_t fade[FADE_OUT_SAMPLES];
        for (int i = 0; i < FADE_OUT_SAMPLES; i++) {
            fade[i] = (int16_t)((int32_t)s_last_sample * (FADE_OUT_SAMPLES - 1 - i) / FADE_OUT_SAMPLES);
        }
        push_audio(fade, FADE_OUT_SAMPLES, s_sentence_generation);
        s_last_sample = 0;
    }
    s_sentence_active = false;
    if (failed) {
        s_failed = true;
    }
    ESP_LOGI(TAG, "\"%.40s%s\": first audio %lld ms, total %lld ms, %u bytes, %s", s_sentence_text,
             strlen(s_sentence_text) > 40 ? "..." : "", s_first_audio_us ? (s_first_audio_us - s_sentence_us) / 1000 : -1LL,
             (esp_timer_get_time() - s_sentence_us) / 1000, (unsigned)s_audio_bytes, status ? status : "?");
}

void tts_backend_no_credit(void)
{
    s_no_credit = true;
}

bool tts_backend_is_no_credit(const char *text)
{
    return text && (strstr(text, "insufficient_quota") || strcasestr(text, "quota") || strcasestr(text, "billing") ||
                    strcasestr(text, "credit"));
}

static void tts_task(void *arg)
{
    /* Nothing to do before the network is up */
    while (WIFI_STATUS_CONNECTED_OK != wifi_connected_already()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    for (;;) {
        /* Settings half changed (the key of another service with the old one...): waited for */
        if (s_configuring) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        /* Another service chosen: the old one closes, the new one connects in advance */
        const tts_backend_t *next = s_next_backend;
        if (next && next != s_backend) {
            if (s_backend) {
                s_backend->stop();
            }
            s_backend = next;
            ESP_LOGI(TAG, "%s speech: %s", s_backend->name, s_settings.model);
            s_backend->start();
        }
        if (s_backend) {
            s_backend->upkeep();
        }

        sentence_item_t item;
        if (xQueueReceive(s_sentences, &item, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        if (item.generation == s_generation) {
            if (item.text && s_backend) {
                s_sentence_text = item.text;
                s_backend->say(item.text, item.generation);
                s_sentence_text = "";
            } else if (item.text) {
                s_failed = true;    // no service: the sentence is lost
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
/*
 * When the synthesis is slower than real time (slow network or service), each underrun doubles the reserve
 * waited for before resuming (100 ms, 200 ms ... 1.6 s): a few longer pauses instead of choppy speech
 */
#define PREBUFFER_RESUME_MAX_SHIFT  4

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
            unsigned shift = underruns < PREBUFFER_RESUME_MAX_SHIFT ? underruns : PREBUFFER_RESUME_MAX_SHIFT;
            size_t needed = started ? (PREBUFFER_RESUME_BYTES << shift) : PREBUFFER_START_BYTES;
            if (available >= needed || (synth_done && available > 0)) {
                playing = true;
            } else {
                if (synth_done && available == 0 && !done_sent) {
                    done_sent = true;
                    ESP_LOGI(TAG, "reply played, %u underrun(s), %u cut(s) mid-sentence%s", underruns,
                             (unsigned)s_cuts, s_failed ? ", sentence(s) lost" : "");
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
                // The speaker is needed (a voice sample of the Settings screen may come while the radio plays)
                radio_suspend();
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
            s_last_play_us = esp_timer_get_time();
        } else {
            /* Ran dry: wait for the end of the reply, or refill the reserve before resuming */
            playing = false;
            if (!synth_done) {
                underruns++;
                /* Not a pause between two sentences: speech cut while a sentence was arriving */
                if (s_sentence_active && s_sentence_has_audio && s_sentence_generation == playing_generation) {
                    s_cuts++;
                }
            }
        }
    }
}

/* ---------- public API ---------- */

const char *const tts_voices[] = {
    "marin", "cedar", "alloy", "ash", "ballad", "coral", "echo", "sage", "shimmer", "verse",
};
const size_t tts_voice_count = sizeof(tts_voices) / sizeof(tts_voices[0]);

/* A setting changed: copied, the version tells the service to apply it */
static void set_setting(const char **field, const char *value)
{
    xSemaphoreTake(s_settings_lock, portMAX_DELAY);
    char *old = (char *)*field;
    char *copy = NULL;
    replace_str(&copy, value);
    *field = copy;
    s_settings.version++;
    xSemaphoreGive(s_settings_lock);
    // The old string may still be read by the synthesis task for a moment: freed a little later
    vTaskDelay(pdMS_TO_TICKS(50));
    free(old);
}

void tts_stream_configure(const tts_backend_t *backend, const char *base_url, const char *api_key, const char *model,
                          const char *voice, const char *instructions)
{
    s_configuring = true;
    set_setting(&s_settings.base_url, base_url);
    set_setting(&s_settings.api_key, api_key);
    set_setting(&s_settings.model, model);
    set_setting(&s_settings.voice, voice);
    if (instructions) {
        set_setting(&s_settings.instructions, instructions);
    }
    ESP_LOGI(TAG, "%s model: %s, voice: %s", backend->name, model ? model : "-", voice ? voice : "-");
    s_next_backend = backend;
    s_configuring = false;
}

void tts_stream_set_voice(const char *voice)
{
    set_setting(&s_settings.voice, voice);
}

void tts_stream_set_instructions(const char *instructions)
{
    set_setting(&s_settings.instructions, instructions);
}

/* One sentence spoken into the capture buffer; true when it gave audio */
static bool test_sentence(const char *text, uint32_t timeout_ms)
{
    s_capture_len = 0;
    s_test_first_ms = -1;
    tts_stream_begin(NULL);
    uint32_t generation = s_generation;
    tts_stream_say(text);
    tts_stream_end();
    for (uint32_t t = 0; t < timeout_ms / 50 && s_synth_done_generation != generation; t++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return s_synth_done_generation == generation && s_test_first_ms >= 0 && s_capture_len > 0;
}

bool tts_stream_test(const tts_backend_t *backend, const char *base_url, const char *api_key, const char *model,
                     const char *voice, const char *text, int16_t **audio, size_t *samples, int *first_ms,
                     int *total_ms)
{
    *audio = NULL;
    *samples = 0;
    *first_ms = *total_ms = -1;
    tts_stream_configure(backend, base_url, api_key, model, voice, NULL);
    for (int i = 0; i < 50 && s_backend != backend; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_capturing = true;
    // A first short sentence opens the connection (as it is open in use), then the one measured
    bool ok = test_sentence(".", 20000) && test_sentence(text, 30000);
    s_capturing = false;
    *first_ms = s_test_first_ms;
    *total_ms = s_test_total_ms;
    if (ok) {
        *audio = s_capture;
        *samples = s_capture_len;
        s_capture = NULL;
        s_capture_len = s_capture_cap = 0;
    }
    return ok;
}

bool tts_stream_no_credit(void)
{
    return s_no_credit;
}

bool tts_stream_reply_was_choppy(void)
{
    return s_failed || s_cuts >= TTS_CHOPPY_CUTS;
}

bool tts_stream_is_silent(uint32_t ms)
{
    return esp_timer_get_time() - s_last_play_us >= (int64_t)ms * 1000;
}

esp_err_t tts_stream_init(void)
{
    ESP_RETURN_ON_FALSE(!s_sentences, ESP_ERR_INVALID_STATE, TAG, "Already initialized");
    resampler_design();
    s_sentences = xQueueCreate(TTS_SENTENCE_QUEUE_LEN, sizeof(sentence_item_t));
    s_audio = xStreamBufferCreateWithCaps(TTS_AUDIO_BUFFER_BYTES, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_settings_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_sentences && s_audio && s_settings_lock, ESP_ERR_NO_MEM, TAG, "Failed to create queues");

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
    s_cuts = 0;
    s_failed = false;
    s_no_credit = false;
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

void tts_stream_cancel(void)
{
    s_generation++;
    drop_queued_sentences();
    s_flush = true;
}
