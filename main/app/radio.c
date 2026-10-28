/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Internet radio. Two tasks, kept from the first stream on (their stacks are in PSRAM):
 * - the network task reads the stream over HTTP into a ring buffer in PSRAM (some seconds of audio, so that the
 *   network may stall without the speaker running dry);
 * - the playback task decodes it (MP3 or AAC, esp_audio_codec), low-pass filters and resamples it to 16 kHz stereo
 *   (the I2S bus, shared with the microphones, stays at 16 kHz) and writes it to the speaker.
 * Phase 0: the decoding and resampling times, the underruns and the internal RAM are logged every 10 s.
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_default.h"
#include "esp_aac_dec.h"
#include "bsp_board.h"
#include "settings.h"
#include "radio.h"

#define RADIO_USER_AGENT        "ESP32-S3-Voice-Assistant/1.0"
#define RADIO_RING_SIZE         (128 * 1024)    /* 8 s at 128 kbit/s */
#define RADIO_PREFILL           (32 * 1024)     /* 2 s at 128 kbit/s before playing */
#define RADIO_PREFILL_WAIT_MS   10000
#define RADIO_CHUNK             2048
#define RADIO_IN_MAX            8192            /* compressed data waiting for the decoder */
#define RADIO_OUT_RATE          16000
#define RADIO_FIR_TAPS          31
#define RADIO_CUTOFF_HZ         7000.0f         /* below the 8 kHz of the 16 kHz output */
#define RADIO_STATS_US          (10 * 1000000)
#define RADIO_HTTP_TIMEOUT_MS   5000
#define RADIO_REDIRECTS_MAX     5
#define RADIO_TREBLE_HZ         3000.0f         /* treble shelf: above this frequency */

static const char *TAG = "radio";

typedef enum {
    FORMAT_MP3,
    FORMAT_AAC,
} radio_format_t;

static RingbufHandle_t s_ring = NULL;
static TaskHandle_t s_net_task = NULL;
static TaskHandle_t s_play_task = NULL;
static char s_url[512];
static char s_content_type[64];
static volatile radio_format_t s_format = FORMAT_MP3;
static volatile bool s_stop = true;
static volatile bool s_net_busy = false;
static volatile bool s_play_busy = false;
static volatile bool s_net_ended = false;

/* ---------- network ---------- */

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(evt->header_key, "Content-Type") == 0) {
        strlcpy(s_content_type, evt->header_value, sizeof(s_content_type));
    }
    return ESP_OK;
}

static void net_stream(void)
{
    s_content_type[0] = '\0';
    esp_http_client_config_t config = {
        .url = s_url,
        .timeout_ms = RADIO_HTTP_TIMEOUT_MS,
        .buffer_size = RADIO_CHUNK,
        .user_agent = RADIO_USER_AGENT,
        .event_handler = http_event,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return;
    }
    int status = 0;
    for (int redirects = 0; redirects <= RADIO_REDIRECTS_MAX && !s_stop; redirects++) {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            ESP_LOGE(TAG, "Could not connect to %s", s_url);
            break;
        }
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            continue;
        }
        break;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d for %s", status, s_url);
        esp_http_client_cleanup(client);
        return;
    }
    s_format = (strcasestr(s_content_type, "aac") || strcasestr(s_content_type, "mp4")) ? FORMAT_AAC : FORMAT_MP3;
    ESP_LOGI(TAG, "Stream: %s (%s)", s_content_type[0] ? s_content_type : "no type",
             s_format == FORMAT_AAC ? "AAC" : "MP3");
    char *chunk = heap_caps_malloc(RADIO_CHUNK, MALLOC_CAP_SPIRAM);
    while (chunk && !s_stop) {
        int n = esp_http_client_read(client, chunk, RADIO_CHUNK);
        if (n <= 0) {
            ESP_LOGW(TAG, "Stream ended (%d)", n);
            break;
        }
        // Waits while the buffer is full (the speaker sets the pace), checking for a stop
        while (!s_stop && xRingbufferSend(s_ring, chunk, n, pdMS_TO_TICKS(200)) != pdTRUE) {
        }
    }
    free(chunk);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

static void net_task(void *arg)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_net_busy = true;
        net_stream();
        s_net_ended = true;
        s_net_busy = false;
    }
}

/*
 * ---------- resampling ----------
 * The speaker is mono: the channels are mixed first. A low-pass FIR (below the 8 kHz of the 16 kHz output) is
 * computed only where an output sample falls (at the two input samples around it, linearly interpolated; at one
 * for a whole ratio such as 48 kHz), in integers, with its state in internal RAM.
 */

#define RS_HISTORY      64                  /* power of 2, > RADIO_FIR_TAPS + the samples between two outputs */
#define RS_FRAC_BITS    16

typedef struct {
    int in_rate;
    int channels;
    int16_t taps[RADIO_FIR_TAPS];           /* Q14 */
    int16_t history[RS_HISTORY];
    uint32_t received;                      /* input samples received */
    uint32_t pos;                           /* next output position, in input samples: integer part */
    uint32_t frac;                          /* and its fraction (RS_FRAC_BITS) */
    uint32_t step;                          /* input samples per output sample (RS_FRAC_BITS) */
} resampler_t;

static resampler_t s_rs;                    /* internal RAM: read at every output sample */

/*
 * Treble: a high-shelf biquad (Audio EQ Cookbook, slope 1) on the 16 kHz mono output, ~5 multiplications per
 * sample. Its coefficients are replaced at once when the setting changes (the state is kept).
 */
typedef struct {
    bool on;
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
} treble_t;

static treble_t s_treble;

void radio_set_treble(int db)
{
    db = db < -12 ? -12 : (db > 6 ? 6 : db);
    if (db == 0) {
        s_treble.on = false;
        return;
    }
    float a = powf(10.0f, db / 40.0f);
    float w0 = 2 * (float)M_PI * RADIO_TREBLE_HZ / RADIO_OUT_RATE;
    float cw = cosf(w0);
    float alpha = sinf(w0) / 2 * sqrtf(2.0f);       // slope S = 1
    float sa = 2 * sqrtf(a) * alpha;
    float a0 = (a + 1) - (a - 1) * cw + sa;
    s_treble.b0 = a * ((a + 1) + (a - 1) * cw + sa) / a0;
    s_treble.b1 = -2 * a * ((a - 1) + (a + 1) * cw) / a0;
    s_treble.b2 = a * ((a + 1) + (a - 1) * cw - sa) / a0;
    s_treble.a1 = 2 * ((a - 1) - (a + 1) * cw) / a0;
    s_treble.a2 = ((a + 1) - (a - 1) * cw - sa) / a0;
    s_treble.on = true;
    ESP_LOGI(TAG, "Treble %+d dB", db);
}

static inline int32_t treble_apply(int32_t sample)
{
    treble_t *t = &s_treble;
    if (!t->on) {
        return sample;
    }
    float x = (float)sample;
    float y = t->b0 * x + t->b1 * t->x1 + t->b2 * t->x2 - t->a1 * t->y1 - t->a2 * t->y2;
    t->x2 = t->x1;
    t->x1 = x;
    t->y2 = t->y1;
    t->y1 = y;
    return (int32_t)y;
}

static void resampler_init(resampler_t *r, int in_rate, int channels)
{
    memset(r, 0, sizeof(*r));
    r->in_rate = in_rate;
    r->channels = channels > 1 ? 2 : 1;
    r->step = (uint32_t)(((uint64_t)in_rate << RS_FRAC_BITS) / RADIO_OUT_RATE);
    // Windowed sinc (Hamming), unity gain
    float h[RADIO_FIR_TAPS];
    float fc = RADIO_CUTOFF_HZ / in_rate;
    float sum = 0;
    for (int i = 0; i < RADIO_FIR_TAPS; i++) {
        int m = i - RADIO_FIR_TAPS / 2;
        float sinc = m == 0 ? 2 * fc : sinf(2 * (float)M_PI * fc * m) / ((float)M_PI * m);
        h[i] = sinc * (0.54f - 0.46f * cosf(2 * (float)M_PI * i / (RADIO_FIR_TAPS - 1)));
        sum += h[i];
    }
    for (int i = 0; i < RADIO_FIR_TAPS; i++) {
        r->taps[i] = (int16_t)lroundf(h[i] / sum * (1 << 14));
    }
}

/* The filtered input at index i (i < received, and still in the history) */
static inline int32_t fir_at(const resampler_t *r, uint32_t i)
{
    int32_t acc = 0;
    for (int k = 0; k < RADIO_FIR_TAPS; k++) {
        acc += (int32_t)r->taps[k] * r->history[(i - k) & (RS_HISTORY - 1)];
    }
    return acc >> 14;
}

static inline int16_t clip16(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

/* in: frames of interleaved 16-bit samples; out: 16 kHz stereo frames (the same sample twice); returns the frames
 * written */
static int resample(resampler_t *r, const int16_t *in, int frames, int16_t *out, int out_max)
{
    int written = 0;
    for (int f = 0; f < frames; f++) {
        int32_t mono = r->channels == 2 ? ((int32_t)in[2 * f] + in[2 * f + 1]) >> 1 : in[f];
        r->history[r->received & (RS_HISTORY - 1)] = (int16_t)mono;
        r->received++;
        // Every output sample whose two neighbours have arrived
        while (r->pos + 1 < r->received && written < out_max) {
            int32_t y = fir_at(r, r->pos);
            if (r->frac) {
                int32_t y1 = fir_at(r, r->pos + 1);
                y += (int32_t)(((int64_t)(y1 - y) * r->frac) >> RS_FRAC_BITS);
            }
            out[2 * written] = out[2 * written + 1] = clip16(treble_apply(y));
            written++;
            r->frac += r->step;
            r->pos += r->frac >> RS_FRAC_BITS;
            r->frac &= (1u << RS_FRAC_BITS) - 1;
        }
    }
    return written;
}

/* ---------- playback ---------- */

static size_t ring_filled(void)
{
    return RADIO_RING_SIZE - xRingbufferGetCurFreeSize(s_ring);
}

static void play_stream(void)
{
    // Some seconds buffered first
    int64_t wait_start = esp_timer_get_time();
    while (!s_stop && ring_filled() < RADIO_PREFILL && !s_net_ended) {
        if (esp_timer_get_time() - wait_start > RADIO_PREFILL_WAIT_MS * 1000LL) {
            ESP_LOGE(TAG, "No audio received in %d s", RADIO_PREFILL_WAIT_MS / 1000);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_stop || ring_filled() == 0) {
        return;
    }

    esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
    aac_cfg.aac_plus_enable = true;
    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = s_format == FORMAT_AAC ? ESP_AUDIO_SIMPLE_DEC_TYPE_AAC : ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = s_format == FORMAT_AAC ? &aac_cfg : NULL,
        .cfg_size = s_format == FORMAT_AAC ? sizeof(aac_cfg) : 0,
    };
    esp_audio_simple_dec_handle_t dec = NULL;
    if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Could not open the decoder");
        return;
    }
    uint8_t *in = heap_caps_malloc(RADIO_IN_MAX, MALLOC_CAP_SPIRAM);
    uint32_t out_size = 4608 * 2;       // an MP3 frame of 1152 stereo samples; grown when needed
    uint8_t *out = heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM);
    int16_t *pcm16k = heap_caps_malloc(2048 * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    resampler_t *rs = &s_rs;
    if (!in || !out || !pcm16k) {
        goto done;
    }

    size_t in_len = 0;
    bool info_known = false;
    int64_t stats_start = esp_timer_get_time(), decode_us = 0, resample_us = 0, audio_frames = 0;
    int underruns = 0, frames_errors = 0;
    size_t min_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    while (!s_stop) {
        // More compressed data
        if (in_len < RADIO_IN_MAX) {
            size_t got = 0;
            uint8_t *item = xRingbufferReceiveUpTo(s_ring, &got, pdMS_TO_TICKS(100), RADIO_IN_MAX - in_len);
            if (item) {
                memcpy(in + in_len, item, got);
                in_len += got;
                vRingbufferReturnItem(s_ring, item);
            } else if (s_net_ended) {
                break;
            } else if (in_len == 0) {
                underruns++;
                continue;
            }
        }
        // Decode what can be
        while (in_len > 0 && !s_stop) {
            esp_audio_simple_dec_raw_t raw = { .buffer = in, .len = in_len };
            esp_audio_simple_dec_out_t frame = { .buffer = out, .len = out_size };
            int64_t t0 = esp_timer_get_time();
            esp_audio_err_t ret = esp_audio_simple_dec_process(dec, &raw, &frame);
            decode_us += esp_timer_get_time() - t0;
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                uint8_t *bigger = heap_caps_realloc(out, frame.needed_size, MALLOC_CAP_SPIRAM);
                if (!bigger) {
                    goto done;
                }
                out = bigger;
                out_size = frame.needed_size;
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK && ret != ESP_AUDIO_ERR_DATA_LACK) {
                frames_errors++;
                raw.consumed = raw.consumed ? raw.consumed : 1;     // skip the bad byte
            }
            if (raw.consumed) {
                memmove(in, in + raw.consumed, in_len - raw.consumed);
                in_len -= raw.consumed;
            }
            if (frame.decoded_size) {
                esp_audio_simple_dec_info_t info = { 0 };
                esp_audio_simple_dec_get_info(dec, &info);
                if (!info_known || info.sample_rate != (uint32_t)rs->in_rate ||
                    (info.channel > 1 ? 2 : 1) != rs->channels) {
                    ESP_LOGI(TAG, "Audio: %" PRIu32 " Hz, %d channel(s), %d bits, %" PRIu32 " bit/s", info.sample_rate,
                             info.channel, info.bits_per_sample, info.bitrate);
                    resampler_init(rs, info.sample_rate, info.channel);
                    info_known = true;
                }
                int frames = frame.decoded_size / (sizeof(int16_t) * rs->channels);
                const int16_t *pcm = (const int16_t *)out;
                while (frames > 0 && !s_stop) {
                    int part = frames > 1024 ? 1024 : frames;
                    int64_t t1 = esp_timer_get_time();
                    int n = resample(rs, pcm, part, pcm16k, 2048);
                    resample_us += esp_timer_get_time() - t1;
                    size_t written = 0;
                    bsp_i2s_write(pcm16k, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
                    audio_frames += n;
                    pcm += part * rs->channels;
                    frames -= part;
                }
            }
            if (!raw.consumed && !frame.decoded_size) {
                break;      // more data needed
            }
        }
        // Phase 0 measures
        size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        min_internal = internal < min_internal ? internal : min_internal;
        int64_t now = esp_timer_get_time();
        if (now - stats_start >= RADIO_STATS_US && audio_frames) {
            double audio_us = audio_frames * 1e6 / RADIO_OUT_RATE;
            ESP_LOGI(TAG, "%.1f s played: decoding %.1f %% of a core, resampling %.1f %%, underruns %d, errors %d, "
                     "buffer %u KB, internal RAM free %u (min %u)", audio_us / 1e6, 100.0 * decode_us / audio_us,
                     100.0 * resample_us / audio_us, underruns, frames_errors, (unsigned)(ring_filled() / 1024),
                     (unsigned)internal, (unsigned)min_internal);
            stats_start = now;
            decode_us = resample_us = audio_frames = 0;
            underruns = frames_errors = 0;
        }
    }
done:
    esp_audio_simple_dec_close(dec);
    free(in);
    free(out);
    free(pcm16k);
}

/* Created for each stream: its stack is in internal RAM (the decoder is much slower with a stack in PSRAM), and
 * given back as soon as the radio stops (a conversation needs it) */
#define RADIO_PLAY_STACK        6144

static void play_task(void *arg)
{
    play_stream();
    ESP_LOGI(TAG, "Playback stopped (stack left %u bytes)", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_play_task = NULL;
    s_play_busy = false;
    vTaskDelete(NULL);
}

/* ---------- interface ---------- */

static bool radio_init(void)
{
    if (s_ring) {
        return true;
    }
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    radio_set_treble(settings_get_parameter()->radio_treble);
    s_ring = xRingbufferCreateWithCaps(RADIO_RING_SIZE, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    if (!s_ring ||
        xTaskCreatePinnedToCoreWithCaps(net_task, "radio_net", 4096, NULL, 4, &s_net_task, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the radio");
        return false;
    }
    return true;
}

void radio_stop(void)
{
    s_stop = true;
    // The speaker is free once the playback task left its loop (a decoded frame at most, ~100 ms)
    for (int i = 0; i < 40 && s_play_busy; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool radio_playing(void)
{
    return !s_stop && s_play_busy;
}

void radio_play(const char *url)
{
    if (!radio_init()) {
        return;
    }
    radio_stop();
    // The network task may still be waiting on the previous stream (HTTP timeout)
    for (int i = 0; i < (RADIO_HTTP_TIMEOUT_MS / 10 + 100) && s_net_busy; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // What is left of the previous stream is dropped
    size_t got = 0;
    void *item;
    while ((item = xRingbufferReceiveUpTo(s_ring, &got, 0, RADIO_RING_SIZE)) != NULL) {
        vRingbufferReturnItem(s_ring, item);
    }
    strlcpy(s_url, url, sizeof(s_url));
    s_net_ended = false;
    s_stop = false;
    ESP_LOGI(TAG, "Playing %s (internal RAM free %u)", s_url, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    xTaskNotifyGive(s_net_task);
    s_play_busy = true;
    if (xTaskCreatePinnedToCore(play_task, "radio_play", RADIO_PLAY_STACK, NULL, 5, &s_play_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "No memory for the playback task");
        s_play_busy = false;
        s_stop = true;
    }
}
