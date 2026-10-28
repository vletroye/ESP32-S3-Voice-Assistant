/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Internet radio. A network task, kept from the first station on (its stack in PSRAM), runs the sessions: it reads
 * the stream over HTTP into a ring buffer in PSRAM (some seconds of audio, so that the network may stall without
 * the speaker running dry), takes the ICY metadata out of it (the title being played), and reconnects when the
 * stream drops. For each stream, a playback task (its stack in internal RAM: the decoder is much slower with a
 * stack in PSRAM; given back as soon as the radio stops, a conversation needing it) decodes it (MP3 or AAC,
 * esp_audio_codec), mixes it to mono, low-pass filters and resamples it to 16 kHz (the I2S bus, shared with the
 * microphones, stays at 16 kHz), softens its treble and writes it to the speaker.
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "esp_crt_bundle.h"
#include "esp_audio_simple_dec.h"
#include "esp_mp3_dec.h"
#include "esp_aac_dec.h"
#include "bsp_board.h"
#include "settings.h"
#include "radio.h"
#include "radio_browser.h"

#define RADIO_USER_AGENT        "ESP32-S3-Voice-Assistant/1.0"
#define RADIO_RING_SIZE         (128 * 1024)    /* 8 s at 128 kbit/s */
#define RADIO_PREFILL           (32 * 1024)     /* 2 s at 128 kbit/s before playing */
#define RADIO_PREFILL_WAIT_MS   10000
#define RADIO_CHUNK             2048
#define RADIO_IN_MAX            8192            /* compressed data waiting for the decoder */
#define RADIO_OUT_RATE          16000
#define RADIO_FIR_TAPS          31
#define RADIO_CUTOFF_HZ         7000.0f         /* below the 8 kHz of the 16 kHz output */
#define RADIO_STATS_US          (60 * 1000000)
#define RADIO_HTTP_TIMEOUT_MS   5000
#define RADIO_REDIRECTS_MAX     5
#define RADIO_TREBLE_HZ         3000.0f         /* treble shelf: above this frequency */
#define RADIO_RETRIES           3               /* reconnections after the stream dropped */
#define RADIO_RETRY_DELAY_MS    2000
#define RADIO_STABLE_US         (30 * 1000000)  /* a stream played this long resets the reconnections */
#define RADIO_PLAY_STACK        4096
#define RADIO_SPEAKER_WAIT_MS   500             /* the playback task leaves within a decoded frame (~100 ms) */
#define RADIO_TITLE_SIZE        128
#define RADIO_META_MAX          255

static const char *TAG = "radio";

typedef enum {
    FORMAT_MP3,
    FORMAT_AAC,
} radio_format_t;

static RingbufHandle_t s_ring = NULL;
static TaskHandle_t s_net_task = NULL;
/* The list played (a search, the favourites, or one station) and the current station of it */
static radio_station_t *s_list = NULL;          /* PSRAM, RADIO_LIST_MAX */
static volatile int s_list_count = 0;
static volatile int s_list_index = 0;
static char s_url[RADIO_URL_SIZE];
static char s_name[RADIO_NAME_SIZE];
static char s_uuid[RADIO_UUID_SIZE];
static char s_title[RADIO_TITLE_SIZE];
static portMUX_TYPE s_title_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_content_type[64];
static char s_icy_name[RADIO_NAME_SIZE];
static volatile int s_icy_metaint = 0;
static volatile radio_format_t s_format = FORMAT_MP3;
static volatile radio_state_t s_state = RADIO_OFF;
static volatile bool s_stop = true;             /* the stream being read and played stops */
static volatile bool s_play_busy = false;       /* the playback task is writing to the speaker */
static volatile bool s_net_ended = false;       /* nothing more will come into the ring buffer */
static volatile bool s_fade_out = false;        /* the playback fades out, then leaves (a stop) */
static volatile uint32_t s_fade_out_samples = 0;    /* the length of that fade-out */
/* A station of the list asked while one plays (next, previous, a favourite touched): the playing one fades out,
 * then the session goes on with this one, without waiting in the caller (the screen stays responsive) */
static volatile int s_pending_index = -1;
/* The assistant listens or speaks (from radio_suspend() to radio_on_sleep()): a station asked meanwhile ("joue du
 * jazz", "suivante") plays once it sleeps again, after its spoken reply */
static volatile bool s_awake = false;
/* A station asked during the conversation: played once the assistant sleeps, even with Radio_resume = off */
static volatile bool s_start_on_sleep = false;

/* ---------- network ---------- */

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER) {
        return ESP_OK;
    }
    if (strcasecmp(evt->header_key, "Content-Type") == 0) {
        strlcpy(s_content_type, evt->header_value, sizeof(s_content_type));
    } else if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
        s_icy_metaint = atoi(evt->header_value);
    } else if (strcasecmp(evt->header_key, "icy-name") == 0) {
        strlcpy(s_icy_name, evt->header_value, sizeof(s_icy_name));
    }
    return ESP_OK;
}

/* Into the ring buffer, waiting while it is full (the speaker sets the pace); false when stopped */
static bool ring_put(const char *data, size_t len)
{
    while (len && !s_stop) {
        if (xRingbufferSend(s_ring, data, len, pdMS_TO_TICKS(200)) == pdTRUE) {
            return true;
        }
    }
    return !s_stop;
}

/* "StreamTitle='Artist - Title';StreamUrl='...';": the title kept */
static void icy_metadata(const char *meta)
{
    const char *start = strstr(meta, "StreamTitle='");
    if (!start) {
        return;
    }
    start += strlen("StreamTitle='");
    const char *end = strstr(start, "';");
    size_t len = end ? (size_t)(end - start) : strlen(start);
    char title[RADIO_TITLE_SIZE];
    snprintf(title, sizeof(title), "%.*s", (int)len, start);
    taskENTER_CRITICAL(&s_title_lock);
    bool changed = strcmp(title, s_title) != 0;
    if (changed) {
        strlcpy(s_title, title, sizeof(s_title));
    }
    taskEXIT_CRITICAL(&s_title_lock);
    if (changed) {
        ESP_LOGI(TAG, "Now playing: %s", title);
    }
}

/* The stream into the ring buffer until it ends or s_stop; true when audio was received */
static bool net_stream(void)
{
    s_content_type[0] = '\0';
    s_icy_name[0] = '\0';
    s_icy_metaint = 0;
    esp_http_client_config_t config = {
        .url = s_url,
        .timeout_ms = RADIO_HTTP_TIMEOUT_MS,
        .buffer_size = RADIO_CHUNK,
        .user_agent = RADIO_USER_AGENT,
        .event_handler = http_event,
        .crt_bundle_attach = esp_crt_bundle_attach,     // https: a favourite added by hand (searches keep http)
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return false;
    }
    esp_http_client_set_header(client, "Icy-MetaData", "1");
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
        if (status) {
            ESP_LOGE(TAG, "HTTP %d for %s", status, s_url);
        }
        esp_http_client_cleanup(client);
        return false;
    }
    s_format = (strcasestr(s_content_type, "aac") || strcasestr(s_content_type, "mp4")) ? FORMAT_AAC : FORMAT_MP3;
    int metaint = s_icy_metaint;
    ESP_LOGI(TAG, "Stream: %s (%s)%s%s, metadata every %d bytes", s_content_type[0] ? s_content_type : "no type",
             s_format == FORMAT_AAC ? "AAC" : "MP3", s_icy_name[0] ? ", " : "", s_icy_name, metaint);
    if (!s_name[0] && s_icy_name[0]) {
        strlcpy(s_name, s_icy_name, sizeof(s_name));
    }

    // The ICY metadata (a length byte, then length x 16 bytes, every metaint bytes of audio) is taken out
    char *chunk = heap_caps_malloc(RADIO_CHUNK, MALLOC_CAP_SPIRAM);
    char *meta = heap_caps_malloc(RADIO_META_MAX + 1, MALLOC_CAP_SPIRAM);
    int to_meta = metaint;      // audio bytes before the next metadata
    int meta_left = -1;         // -1: audio; -2: the length byte comes next; else metadata bytes still to read
    int meta_len = 0;
    bool received = false;
    while (chunk && meta && !s_stop && !(s_pending_index >= 0 && !s_play_busy)) {
        int n = esp_http_client_read(client, chunk, RADIO_CHUNK);
        if (n <= 0) {
            ESP_LOGW(TAG, "Stream ended (%d)", n);
            break;
        }
        received = true;
        for (int i = 0; i < n && !s_stop;) {
            if (metaint <= 0) {
                ring_put(chunk + i, n - i);
                i = n;
            } else if (meta_left == -1) {
                int part = (n - i) < to_meta ? (n - i) : to_meta;
                ring_put(chunk + i, part);
                i += part;
                to_meta -= part;
                if (to_meta == 0) {
                    meta_left = -2;
                }
            } else if (meta_left == -2) {
                meta_left = (unsigned char)chunk[i++] * 16;
                meta_len = 0;
                if (meta_left == 0) {
                    meta_left = -1;
                    to_meta = metaint;
                }
            } else {
                int part = (n - i) < meta_left ? (n - i) : meta_left;
                int keep = (meta_len + part) > RADIO_META_MAX ? RADIO_META_MAX - meta_len : part;
                memcpy(meta + meta_len, chunk + i, keep);
                meta_len += keep;
                i += part;
                meta_left -= part;
                if (meta_left == 0) {
                    meta[meta_len] = '\0';
                    icy_metadata(meta);
                    meta_left = -1;
                    to_meta = metaint;
                }
            }
        }
    }
    free(chunk);
    free(meta);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return received;
}

static void play_task(void *arg);

static bool wait_speaker_free(int ms)
{
    for (int i = 0; i < ms / 10 && s_play_busy; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return !s_play_busy;
}

static void set_current(int index);

/*
 * A session: the stream played until stopped (suspend, pause, another station); reconnected when it drops. A
 * station of a list that gives no audio at all is skipped: the next one is played.
 */
static void net_session(void)
{
    int failures = 0;
    int skipped = 0;
    char clicked[RADIO_UUID_SIZE] = "";
    while (s_state == RADIO_PLAYING) {
        // A listen counted by Radio-Browser, once per station played
        if (s_uuid[0] && strcmp(clicked, s_uuid) != 0) {
            strlcpy(clicked, s_uuid, sizeof(clicked));
            radio_browser_click(s_uuid);
            // (s_stop is still set by the request that started this session: only a pause or a suspension ends it)
            if (s_state != RADIO_PLAYING) {
                return;
            }
        }
        // The previous playback has left the speaker, and what it left in the buffer is dropped
        wait_speaker_free(2000);
        size_t got = 0;
        void *item;
        while ((item = xRingbufferReceiveUpTo(s_ring, &got, 0, RADIO_RING_SIZE)) != NULL) {
            vRingbufferReturnItem(s_ring, item);
        }
        taskENTER_CRITICAL(&s_title_lock);
        s_title[0] = '\0';
        taskEXIT_CRITICAL(&s_title_lock);
        s_net_ended = false;
        s_fade_out = false;
        s_stop = false;
        ESP_LOGI(TAG, "Playing %s: %s (internal RAM free %u)", s_name[0] ? s_name : "?", s_url,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        s_play_busy = true;
        if (xTaskCreatePinnedToCore(play_task, "radio_play", RADIO_PLAY_STACK, NULL, 5, NULL, 1) != pdPASS) {
            ESP_LOGE(TAG, "No memory for the playback task");
            s_play_busy = false;
            s_state = RADIO_OFF;
            return;
        }
        int64_t start = esp_timer_get_time();
        bool received = net_stream();
        s_net_ended = true;
        if (s_stop) {
            return;     // stopped on purpose: the caller set the new state
        }
        if (s_pending_index >= 0) {
            // Another station of the list asked (the playing one has faded out): it is played now
            wait_speaker_free(RADIO_SPEAKER_WAIT_MS);
            set_current(s_pending_index);
            s_pending_index = -1;
            failures = 0;
            skipped = 0;
            continue;
        }
        // The stream dropped (or never came): the playback ends with the buffer, then it is tried again
        wait_speaker_free(RADIO_RING_SIZE / 16 * 2);    // what was buffered is played
        if (received && esp_timer_get_time() - start > RADIO_STABLE_US) {
            failures = 0;
        }
        if (!received && s_list_count > 1 && skipped < s_list_count - 1) {
            // Nothing from this station: the next one of the list
            skipped++;
            ESP_LOGW(TAG, "%s gives no audio: skipped", s_name[0] ? s_name : s_url);
            set_current((s_list_index + 1) % s_list_count);
            failures = 0;
            continue;
        }
        if (++failures > RADIO_RETRIES) {
            ESP_LOGE(TAG, "%s could not be played", s_name[0] ? s_name : s_url);
            s_state = RADIO_OFF;
            return;
        }
        ESP_LOGW(TAG, "Reconnecting (%d/%d)", failures, RADIO_RETRIES);
        for (int i = 0; i < RADIO_RETRY_DELAY_MS / 100 && s_state == RADIO_PLAYING; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static void net_task(void *arg)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        net_session();
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

/*
 * Level of the output, after the treble:
 * - a fade-in at each start (a station, a resumption): from silence to the full level in RADIO_FADE_MS, along a
 *   square curve (soft at first, then faster), which sounds natural;
 * - the stations brought to a common level: the mean square of the sound (blocks of 50 ms, silences left out) is
 *   smoothed over ~3 s and a gain aims at Radio_level (CONFIG.INI), between RADIO_GAIN_MIN_DB and RADIO_GAIN_MAX_DB,
 *   moving slowly (no pumping on the music); fast during the fade-in, which hides it. Peaks pushed over the top
 *   by a gain are softened before the hard clip.
 * The volume of the codec (Volume, "parle plus fort") still applies on top.
 */
#define RADIO_FADE_MS           2000
#define RADIO_FADE_SAMPLES      (RADIO_OUT_RATE * RADIO_FADE_MS / 1000)
#define RADIO_GAIN_MIN_DB       (-12.0f)
#define RADIO_GAIN_MAX_DB       6.0f
#define RADIO_SILENCE_DBFS      (-45.0f)        /* quieter blocks are not measured */
#define RADIO_LEVEL_BLOCK       (RADIO_OUT_RATE / 20)   /* 50 ms */
#define RADIO_LEVEL_FAST_BLOCKS 20              /* the first second: fast */
#define RADIO_SOFT_KNEE         24000
#define RADIO_FADE_OUT_MS       400             /* at a stop: short, the assistant stays quick to answer */
#define RADIO_FADE_CHANGE_MS    1000            /* at a change of station: longer, softer */

typedef struct {
    uint32_t fade;          /* samples played since the start */
    float sum;              /* squares of the block being measured */
    int count;
    int blocks;             /* blocks measured (not silent) since the start */
    float loudness;         /* mean square, smoothed */
    float gain;             /* linear, applied: moves to gain_target sample by sample (no step, no click) */
    float gain_target;
    float target_ms, silence_ms, gain_min, gain_max;
    uint32_t fade_out;      /* samples played since the fade-out began */
    volatile bool faded_out;    /* silent: the playback can leave */
} level_t;

static level_t s_level;     /* internal RAM: read at every output sample */

static float dbfs_to_ms(float db)
{
    float amplitude = 32767.0f * powf(10.0f, db / 20.0f);
    return amplitude * amplitude;
}

static void level_reset(void)
{
    memset(&s_level, 0, sizeof(s_level));
    s_level.gain = 1.0f;
    s_level.gain_target = 1.0f;
    s_level.target_ms = dbfs_to_ms((float)settings_get_parameter()->radio_level);     // Radio_level
    s_level.silence_ms = dbfs_to_ms(RADIO_SILENCE_DBFS);
    s_level.gain_min = powf(10.0f, RADIO_GAIN_MIN_DB / 20.0f);
    s_level.gain_max = powf(10.0f, RADIO_GAIN_MAX_DB / 20.0f);
}

/* The station's level in dBFS and the gain applied in dB (for the log) */
static void level_read(float *dbfs, float *gain_db)
{
    *dbfs = s_level.loudness > 0 ? 10.0f * log10f(s_level.loudness / (32767.0f * 32767.0f)) : -99.0f;
    *gain_db = 20.0f * log10f(s_level.gain);
}

static inline int32_t level_apply(int32_t sample)
{
    level_t *l = &s_level;
    float x = (float)sample;
    l->sum += x * x;
    if (++l->count == RADIO_LEVEL_BLOCK) {
        float ms = l->sum / RADIO_LEVEL_BLOCK;
        l->sum = 0;
        l->count = 0;
        if (ms > l->silence_ms) {
            bool fast = l->blocks < RADIO_LEVEL_FAST_BLOCKS;
            l->loudness = l->blocks == 0 ? ms : l->loudness + (fast ? 0.3f : 0.0165f) * (ms - l->loudness);
            l->blocks++;
            float target = sqrtf(l->target_ms / l->loudness);
            target = target < l->gain_min ? l->gain_min : (target > l->gain_max ? l->gain_max : target);
            l->gain_target += (fast ? 0.5f : 0.05f) * (target - l->gain_target);
            if (l->blocks == 3 * 20) {      // after 3 s of sound
                float dbfs, gain_db;
                level_read(&dbfs, &gain_db);
                ESP_LOGI(TAG, "Level %.1f dBFS: gain %+.1f dB", dbfs, gain_db);
            }
        }
    }
    l->gain += (l->gain_target - l->gain) * (1.0f / RADIO_LEVEL_BLOCK);
    float v = x * l->gain;
    if (l->fade < RADIO_FADE_SAMPLES) {
        float t = (float)l->fade++ / RADIO_FADE_SAMPLES;
        v *= t * t;
    }
    // Fade-out at a stop: the reverse curve, from the level reached (even in the middle of the fade-in)
    if (s_fade_out) {
        if (l->fade_out >= s_fade_out_samples) {
            l->faded_out = true;
            return 0;
        }
        float t = 1.0f - (float)l->fade_out++ / s_fade_out_samples;
        v *= t * t;
    }
    // Soft knee above RADIO_SOFT_KNEE (a gain may push the peaks over the top)
    if (v > RADIO_SOFT_KNEE) {
        v = RADIO_SOFT_KNEE + (v - RADIO_SOFT_KNEE) * 0.25f;
    } else if (v < -RADIO_SOFT_KNEE) {
        v = -RADIO_SOFT_KNEE + (v + RADIO_SOFT_KNEE) * 0.25f;
    }
    return (int32_t)v;
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
            out[2 * written] = out[2 * written + 1] = clip16(level_apply(treble_apply(y)));
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
    while (!s_stop && !s_fade_out && ring_filled() < RADIO_PREFILL && !s_net_ended) {
        if (esp_timer_get_time() - wait_start > RADIO_PREFILL_WAIT_MS * 1000LL) {
            ESP_LOGE(TAG, "No audio received in %d s", RADIO_PREFILL_WAIT_MS / 1000);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_stop || s_fade_out || ring_filled() == 0) {
        return;
    }
    level_reset();      // fade-in, and the level of this station measured afresh

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
    while (!s_stop && !s_level.faded_out) {
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
        while (in_len > 0 && !s_stop && !s_level.faded_out) {
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
                while (frames > 0 && !s_stop && !s_level.faded_out) {
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
        // Measures, once a minute
        size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        min_internal = internal < min_internal ? internal : min_internal;
        int64_t now = esp_timer_get_time();
        if (now - stats_start >= RADIO_STATS_US && audio_frames) {
            double audio_us = audio_frames * 1e6 / RADIO_OUT_RATE;
            float dbfs, gain_db;
            level_read(&dbfs, &gain_db);
            ESP_LOGI(TAG, "%.1f s played: decoding %.1f %% of a core, resampling %.1f %%, underruns %d, errors %d, "
                     "buffer %u KB, level %.1f dBFS (gain %+.1f dB), internal RAM free %u (min %u)", audio_us / 1e6,
                     100.0 * decode_us / audio_us, 100.0 * resample_us / audio_us, underruns, frames_errors,
                     (unsigned)(ring_filled() / 1024), dbfs, gain_db, (unsigned)internal, (unsigned)min_internal);
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

/* Created for each stream by the session (stack in internal RAM), deleted at its end */
static void play_task(void *arg)
{
    play_stream();
    ESP_LOGI(TAG, "Playback stopped (stack left %u bytes)", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_play_busy = false;
    vTaskDelete(NULL);
}

/* ---------- interface ---------- */

static bool radio_init(void)
{
    if (s_ring) {
        return true;
    }
    s_list = heap_caps_calloc(RADIO_LIST_MAX, sizeof(radio_station_t), MALLOC_CAP_SPIRAM);
    if (!s_list) {
        return false;
    }
    // MP3 and AAC (HE-AAC included) only: the other decoders of the component are not linked
    esp_mp3_dec_register();
    esp_aac_dec_register();
    radio_set_treble(settings_get_parameter()->radio_treble);
    s_ring = xRingbufferCreateWithCaps(RADIO_RING_SIZE, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    if (!s_ring ||
        xTaskCreatePinnedToCoreWithCaps(net_task, "radio_net", 4096, NULL, 4, &s_net_task, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the radio");
        return false;
    }
    return true;
}

/* The stream stops (the session task sees it), the speaker is free once the playback task left */
static void stream_stop_fade(int fade_ms)
{
    // A change of station still pending (a wake word right after "next"): that station becomes the current one
    if (s_pending_index >= 0) {
        set_current(s_pending_index);
        s_pending_index = -1;
    }
    // Playing: faded out first (the network keeps feeding it meanwhile), then stopped
    if (s_play_busy) {
        s_fade_out_samples = RADIO_OUT_RATE * fade_ms / 1000;
        s_fade_out = true;
        wait_speaker_free(fade_ms + 200);
    }
    s_stop = true;
    if (!wait_speaker_free(RADIO_SPEAKER_WAIT_MS)) {
        ESP_LOGW(TAG, "The playback did not stop in %d ms", RADIO_SPEAKER_WAIT_MS);
    }
    s_fade_out = false;
}

static void stream_stop(void)
{
    stream_stop_fade(RADIO_FADE_OUT_MS);
}

/* A station of the list while one plays and the assistant sleeps: changed without waiting (the session does it once
 * the playing one has faded out); false when it has to be done here */
static bool change_async(int index)
{
    if (s_state != RADIO_PLAYING || !s_play_busy || s_awake) {
        return false;
    }
    s_fade_out_samples = RADIO_OUT_RATE * RADIO_FADE_CHANGE_MS / 1000;
    s_pending_index = index;
    s_fade_out = true;
    return true;
}

static void session_start(void)
{
    if (s_awake) {
        s_state = RADIO_SUSPENDED;  // played by radio_on_sleep()
        s_start_on_sleep = true;
        return;
    }
    s_state = RADIO_PLAYING;
    s_stop = true;              // the stream being played, if any, ends: the session task starts the new one
    xTaskNotifyGive(s_net_task);
}

/* The station index of the list becomes the current one */
static void set_current(int index)
{
    s_list_index = index;
    strlcpy(s_name, s_list[index].name, sizeof(s_name));
    strlcpy(s_url, s_list[index].url, sizeof(s_url));
    strlcpy(s_uuid, s_list[index].uuid, sizeof(s_uuid));
}

void radio_play_list(const radio_station_t *list, int count, int start)
{
    if (!list || count <= 0 || !radio_init()) {
        return;
    }
    count = count > RADIO_LIST_MAX ? RADIO_LIST_MAX : count;
    start = start >= 0 && start < count ? start : 0;
    if (s_state == RADIO_PLAYING && s_play_busy && !s_awake) {
        // Playing: the new list replaces the old one, the session moves to its station after the fade-out
        memcpy(s_list, list, count * sizeof(radio_station_t));
        s_list_count = count;
        change_async(start);
        return;
    }
    stream_stop_fade(RADIO_FADE_CHANGE_MS);
    memcpy(s_list, list, count * sizeof(radio_station_t));
    s_list_count = count;
    set_current(start);
    session_start();
}

void radio_play(const char *name, const char *url)
{
    if (!url || !url[0]) {
        return;
    }
    radio_station_t station = { 0 };
    strlcpy(station.name, name ? name : "", sizeof(station.name));
    strlcpy(station.url, url, sizeof(station.url));
    radio_play_list(&station, 1, 0);
}

/* The previous / next station, the list looping (after the last one, the first) */
static bool move(int delta)
{
    if (!s_list || s_list_count < 2) {
        return false;
    }
    int base = s_pending_index >= 0 ? s_pending_index : s_list_index;     // pressed again before the change
    int index = (base + delta + s_list_count) % s_list_count;
    if (change_async(index)) {
        return true;
    }
    stream_stop_fade(RADIO_FADE_CHANGE_MS);
    set_current(index);
    session_start();
    return true;
}

bool radio_next(void)
{
    return move(1);
}

bool radio_previous(void)
{
    return move(-1);
}

int radio_list_index(void)
{
    return s_list_index;
}

int radio_list_count(void)
{
    return s_list_count;
}

radio_station_t radio_station(void)
{
    radio_station_t station = { 0 };
    strlcpy(station.name, s_name, sizeof(station.name));
    strlcpy(station.url, s_url, sizeof(station.url));
    strlcpy(station.uuid, s_uuid, sizeof(station.uuid));
    return station;
}

void radio_suspend(void)
{
    s_awake = true;
    if (s_state == RADIO_PLAYING) {
        s_start_on_sleep = false;   // interrupted: played again afterwards only with Radio_resume = on
        s_state = RADIO_SUSPENDED;
        ESP_LOGI(TAG, "Suspended");
    }
    if (s_ring) {
        stream_stop();
    }
}

void radio_on_sleep(void)
{
    s_awake = false;
    bool asked = s_start_on_sleep;
    s_start_on_sleep = false;
    if (s_state == RADIO_SUSPENDED && !asked && !settings_get_parameter()->radio_resume) {
        s_state = RADIO_PAUSED;     // Radio_resume = off: "remets la radio" plays it again
        ESP_LOGI(TAG, "Not resumed (Radio_resume = off)");
        return;
    }
    if (s_state == RADIO_SUSPENDED) {
        ESP_LOGI(TAG, "Resumed (the assistant sleeps)");
        session_start();
    }
}

void radio_pause(void)
{
    if (s_state == RADIO_PLAYING || s_state == RADIO_SUSPENDED) {
        s_state = RADIO_PAUSED;
        ESP_LOGI(TAG, "Paused");
    }
    if (s_ring) {
        stream_stop();
    }
}

bool radio_resume(void)
{
    if (!s_url[0] || !radio_init()) {
        return false;
    }
    if (s_state != RADIO_PLAYING) {
        session_start();
    }
    return true;
}

radio_state_t radio_state(void)
{
    return s_state;
}

bool radio_playing(void)
{
    return s_state == RADIO_PLAYING;
}

const char *radio_station_name(void)
{
    return s_name;
}

void radio_title(char *buf, size_t size)
{
    taskENTER_CRITICAL(&s_title_lock);
    strlcpy(buf, s_title, size);
    taskEXIT_CRITICAL(&s_title_lock);
}
