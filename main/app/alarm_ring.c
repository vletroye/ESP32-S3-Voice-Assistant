/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "bsp_board.h"
#include "settings.h"
#include "file_lang.h"
#include "app_presence.h"
#include "app_ui_ctrl.h"
#include "app_volume.h"
#include "tts_stream.h"
#include "radio.h"
#include "radio_browser.h"
#include "alarm_ring.h"

/* main.c */
extern void assistant_show(const char *text);
extern void assistant_say(const char *text);
extern void assistant_announce_begin(void);
extern void assistant_announce(const char *text);
extern void assistant_announce_end(void);

#define RATE                16000
/* The chime: two bell notes, then a pause; made once, played again and again */
#define CHIME_MS            2200
#define CHIME_FRAMES        (RATE * CHIME_MS / 1000)
#define CHIME_PEAK          16000.0f
/* Written 20 ms at a time (the I2S DMA holds ~80 ms): a stop is heard at once */
#define PIECE_FRAMES        (RATE / 50)
/* A reminder's chime, at the volume set */
#define REMINDER_GAIN       0.5f

static const char *TAG = "alarm_ring";

typedef struct {
    alarm_t alarm;
    bool missed;
} ring_request_t;

static QueueHandle_t s_requests = NULL;
static int16_t *s_chime = NULL;         /* stereo, PSRAM */
static volatile bool s_active = false;
static volatile bool s_stop = false;
static volatile bool s_writing = false; /* the chime is being written to the speaker */
static alarm_t s_last;                  /* the last one rung, for a snooze */
static int64_t s_last_us = 0;

/* A bell note: a few partials, a quick attack and an exponential decay */
static void chime_note(float *mix, float freq, float start_s, float length_s, float decay_s)
{
    static const float partials[][2] = { { 1.0f, 1.0f }, { 2.0f, 0.35f }, { 3.01f, 0.12f } };
    size_t first = (size_t)(start_s * RATE);
    size_t count = (size_t)(length_s * RATE);
    for (size_t i = 0; i < count && first + i < CHIME_FRAMES; i++) {
        float t = (float)i / RATE;
        float envelope = (t < 0.005f ? t / 0.005f : 1.0f) * expf(-t / decay_s);
        float v = 0;
        for (size_t p = 0; p < sizeof(partials) / sizeof(partials[0]); p++) {
            v += partials[p][1] * sinf(2.0f * (float)M_PI * freq * partials[p][0] * t);
        }
        mix[first + i] += v * envelope;
    }
}

static void chime_make(void)
{
    float *mix = heap_caps_calloc(CHIME_FRAMES, sizeof(float), MALLOC_CAP_SPIRAM);
    s_chime = heap_caps_malloc(CHIME_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!mix || !s_chime) {
        free(mix);
        return;
    }
    chime_note(mix, 880.0f, 0.0f, 0.9f, 0.30f);
    chime_note(mix, 659.3f, 0.45f, 1.6f, 0.45f);
    float peak = 0;
    for (size_t i = 0; i < CHIME_FRAMES; i++) {
        peak = fabsf(mix[i]) > peak ? fabsf(mix[i]) : peak;
    }
    for (size_t i = 0; i < CHIME_FRAMES; i++) {
        int16_t v = (int16_t)(mix[i] / peak * CHIME_PEAK);
        s_chime[2 * i] = v;
        s_chime[2 * i + 1] = v;
    }
    free(mix);
}

/*
 * The chime from `*pos`, one piece of 20 ms at this gain (0..1); false once stopped. Waits for the speaker to be
 * silent first (a reply ending).
 */
static bool chime_piece(size_t *pos, float gain)
{
    EXT_RAM_BSS_ATTR static int16_t piece[PIECE_FRAMES * 2];
    if (s_stop || !s_chime) {
        return false;
    }
    s_writing = true;
    for (size_t i = 0; i < PIECE_FRAMES; i++) {
        size_t at = (*pos + i) % CHIME_FRAMES;
        piece[2 * i] = (int16_t)(s_chime[2 * at] * gain);
        piece[2 * i + 1] = (int16_t)(s_chime[2 * at + 1] * gain);
    }
    size_t written = 0;
    bsp_i2s_write(piece, sizeof(piece), &written, portMAX_DELAY);
    s_writing = false;
    *pos = (*pos + PIECE_FRAMES) % CHIME_FRAMES;
    return !s_stop;
}

/* "07:30" */
static void time_text(time_t when, char *out, size_t size)
{
    struct tm t;
    localtime_r(&when, &t);
    strftime(out, size, "%H:%M", &t);
}

/* What is shown and said when it rings */
static void ring_text(const alarm_t *a, bool missed, char *out, size_t size)
{
    const app_lang_t *lang = app_lang_get();
    char when[8];
    time_text(a->due, when, sizeof(when));
    if (missed) {
        if (a->label[0]) {
            snprintf(out, size, lang->alarm_missed, when, a->label);
        } else {
            snprintf(out, size, lang->alarm_missed_plain, when);
        }
    } else if (a->kind == ALARM_KIND_REMINDER) {
        snprintf(out, size, lang->alarm_reminder, a->label);
    } else if (a->kind == ALARM_KIND_TIMER) {
        if (a->label[0]) {
            snprintf(out, size, lang->alarm_timer_label, a->label);
        } else {
            strlcpy(out, lang->alarm_timer, size);
        }
    } else if (a->label[0]) {
        snprintf(out, size, lang->alarm_alarm_label, when, a->label);
    } else {
        snprintf(out, size, lang->alarm_alarm, when);
    }
}

/* A timer or an alarm: the chime (or its radio) rising from silence, its name said once at the full level */
static void ring_loud(const alarm_t *a, const char *text)
{
    const int64_t t0 = esp_timer_get_time();
    const int64_t stop_us = (int64_t)settings_get_parameter()->alarm_stop_min * 60 * 1000000LL;
    const int64_t ramp_us = ALARM_RAMP_S * 1000000LL;
    bsp_codec_volume_set(ALARM_RING_VOLUME, NULL);      // the speaker is silent: no crackle

    bool radio = false;
    if (a->radio[0]) {
        // Its stream found when it was set; else the directory, tried a few times (it may not answer once)
        radio_station_t station = { 0 };
        strlcpy(station.name, a->radio, sizeof(station.name));
        strlcpy(station.url, a->radio_url, sizeof(station.url));
        radio_station_t *found = station.url[0] ? NULL : calloc(RADIO_BROWSER_MAX, sizeof(radio_station_t));
        for (int attempt = 0; found && attempt < 3 && !s_stop; attempt++) {
            int n = radio_browser_search(a->radio, NULL, NULL, found, RADIO_BROWSER_MAX);
            if (n > 0) {
                station = found[radio_browser_best(a->radio, found, n)];
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        free(found);
        if (station.url[0] && !s_stop) {
            radio_set_next_fade_in(ALARM_RAMP_S * 1000);
            radio_play_now(station.name, station.url);
            radio = true;
        }
        ESP_LOGI(TAG, "radio \"%s\": %s", a->radio, radio ? station.name : "not found, the chime instead");
    }
    size_t pos = 0;
    bool said = !a->label[0] || radio;      // a radio is not talked over: its name only shown
    while (!s_stop && esp_timer_get_time() - t0 < stop_us) {
        int64_t elapsed = esp_timer_get_time() - t0;
        if (radio) {
            if (radio_state() == RADIO_OFF) {
                ESP_LOGW(TAG, "the radio stopped: the chime instead");
                radio = false;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!said && elapsed >= ramp_us) {
            // Its name, between two chimes
            said = true;
            assistant_announce_begin();
            assistant_announce(text);
            assistant_announce_end();
            continue;
        }
        float x = elapsed >= ramp_us ? 1.0f : (float)elapsed / ramp_us;
        if (!chime_piece(&pos, x * x)) {    // a square curve: soft at first, then faster
            break;
        }
    }
    if (radio || a->radio[0]) {
        radio_pause();
    }
    ESP_LOGI(TAG, "%s after %lld s", s_stop ? "stopped" : "stopped by itself (Alarm_stop_min)",
             (esp_timer_get_time() - t0) / 1000000);
    // The volume set, once the speaker is silent again
    for (int i = 0; i < 50 && !tts_stream_is_silent(200); i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    bsp_codec_volume_set(app_volume_get(), NULL);
}

static void ring(const alarm_t *a, bool missed)
{
    char text[ALARM_LABEL_SIZE + 96];
    ring_text(a, missed, text, sizeof(text));
    ESP_LOGI(TAG, "%s", text);
    app_presence_activity();        // the screen on
    radio_suspend();
    if (missed || a->kind == ALARM_KIND_REMINDER) {
        if (!missed) {
            size_t pos = 0;
            for (size_t n = 0; n < CHIME_FRAMES / PIECE_FRAMES && chime_piece(&pos, REMINDER_GAIN); n++) {
            }
        }
        // Shown, said, then listened to as a reply ("rappelle-le-moi dans 10 minutes")
        if (!s_stop) {
            assistant_say(text);
        }
        return;
    }
    assistant_show(text);
    ring_loud(a, text);
    if (!s_stop) {
        // Stopped by itself: back to sleep (a wake word or a touch listens instead)
        ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, 0);
    }
}

static void ring_task(void *arg)
{
    ring_request_t request;
    for (;;) {
        if (xQueueReceive(s_requests, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        s_stop = false;
        s_active = true;
        s_last = request.alarm;
        s_last_us = esp_timer_get_time();
        ring(&request.alarm, request.missed);
        s_active = false;
    }
}

void alarm_ring_init(void)
{
    chime_make();
    s_requests = xQueueCreateWithCaps(4, sizeof(ring_request_t), MALLOC_CAP_SPIRAM);
    // PSRAM stack, as the speech's playback task that also writes to the I2S and the codec: the internal RAM is
    // short, TLS needs it for the calls to the services
    if (xTaskCreatePinnedToCoreWithCaps(ring_task, "alarm_ring", 6 * 1024, NULL, 5, NULL, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
    }
}

bool alarm_ring_start(const alarm_t *alarm, bool missed)
{
    if (!s_requests || s_active) {
        return false;
    }
    ring_request_t request = { .alarm = *alarm, .missed = missed };
    s_active = true;        // at once: the main task does not take the next one meanwhile
    return xQueueSend(s_requests, &request, 0) == pdTRUE;
}

bool alarm_ring_active(void)
{
    return s_active;
}

bool alarm_ring_stop(void)
{
    if (!s_active) {
        return false;
    }
    s_stop = true;
    // The piece being written ends (20 ms), then the speaker is the assistant's
    for (int i = 0; i < 20 && s_writing; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "stop asked");
    return true;
}

bool alarm_ring_snooze(int minutes)
{
    if (!s_last_us || (!s_active && esp_timer_get_time() - s_last_us > ALARM_SNOOZE_WINDOW_S * 1000000LL)) {
        return false;
    }
    alarm_t again = s_last;
    alarm_ring_stop();
    return alarms_snooze(&again, minutes > 0 ? minutes : 5);
}
