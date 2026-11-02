/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Debug traces: the logs caught (esp_log_set_vprintf), each line prefixed with the time of day and the number of
 * the exchange, kept in a ring buffer in PSRAM; written to SPIFFS only when dumped.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "debug_trace.h"
#include "app_volume.h"

static const char *TAG = "trace";

#define TRACE_RING_SIZE     (1024 * 1024)   /* about 10 000 lines: hours of tests */
#define TRACE_LINE_MAX      400
#define TRACE_SPIFFS_SPARE  (64 * 1024)     /* left free on SPIFFS by the dump */
#define TRACE_DUMP_PIECE    4096

static char *s_ring = NULL;                 /* PSRAM */
static size_t s_pos = 0;                    /* where the next byte goes */
static bool s_wrapped = false;
static size_t s_lines = 0;                  /* written since the start (the oldest may be dropped) */
static SemaphoreHandle_t s_lock = NULL;
static vprintf_like_t s_prev = NULL;
static volatile bool s_on = false;
static volatile int s_turn = 0;
static volatile int s_marks = 0;
static char *s_line = NULL;                 /* the line being made, under s_lock */
static size_t s_line_len = 0;               /* of a line printed in several calls (Wi-Fi: "I (12) wifi:", then the text) */

static void ring_put(const char *text, size_t len)
{
    while (len > 0) {
        size_t n = TRACE_RING_SIZE - s_pos < len ? TRACE_RING_SIZE - s_pos : len;
        memcpy(s_ring + s_pos, text, n);
        s_pos += n;
        text += n;
        len -= n;
        if (s_pos == TRACE_RING_SIZE) {
            s_pos = 0;
            s_wrapped = true;
        }
    }
}

/* The bytes of broken UTF-8 characters (a text cut by "%.40s" in the middle of an "é") left out; the new length */
static size_t utf8_valid(char *s, size_t len)
{
    size_t w = 0;
    for (size_t r = 0; r < len;) {
        unsigned char c = (unsigned char)s[r];
        size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        bool ok = n > 0 && r + n <= len;
        for (size_t i = 1; ok && i < n; i++) {
            ok = ((unsigned char)s[r + i] & 0xC0) == 0x80;
        }
        if (!ok) {
            r++;
            continue;
        }
        memmove(s + w, s + r, n);
        w += n;
        r += n;
    }
    s[w] = '\0';
    return w;
}

/* "14:32:05.123", or the time since the start ("+12.345") before the clock is set */
static void time_prefix(char *buf, size_t size)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec > 1700000000) {
        struct tm tm;
        localtime_r(&tv.tv_sec, &tm);
        snprintf(buf, size, "%02d:%02d:%02d.%03ld", tm.tm_hour, tm.tm_min, tm.tm_sec, (long)(tv.tv_usec / 1000));
    } else {
        int64_t ms = esp_timer_get_time() / 1000;
        snprintf(buf, size, "+%lld.%03lld", ms / 1000, ms % 1000);
    }
}

/*
 * A log line: printed as before, then kept without its colours nor its time since the start ("I (12345) tts: x"
 * becomes "14:32:05.123 #12 I tts: x")
 */
static int trace_vprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int ret = s_prev ? s_prev(fmt, args) : vprintf(fmt, args);
    if (s_on && s_ring && !xPortInIsrContext() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
            xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        int n = vsnprintf(s_line + s_line_len, TRACE_LINE_MAX - s_line_len, fmt, copy);
        size_t len = s_line_len + (n > 0 ? (size_t)n : 0);
        len = len < TRACE_LINE_MAX ? len : TRACE_LINE_MAX - 1;
        // A line without its end yet: kept until the next call ends it
        bool whole = len == 0 || s_line[len - 1] == '\n' || len == TRACE_LINE_MAX - 1;
        s_line_len = whole ? 0 : len;
        if (n > 0 && whole) {
            // The colours ("\033[0;32m") out
            size_t w = 0;
            for (size_t r = 0; r < len; r++) {
                if (s_line[r] == '\033') {
                    while (r < len && s_line[r] != 'm') {
                        r++;
                    }
                    continue;
                }
                s_line[w++] = s_line[r];
            }
            s_line[w] = '\0';
            w = utf8_valid(s_line, w);
            char *text = s_line;
            // "I (12345) " -> "I "
            if (text[0] && text[1] == ' ' && text[2] == '(') {
                char *close = strchr(text + 3, ')');
                if (close && close[1] == ' ') {
                    memmove(text + 2, close + 2, strlen(close + 2) + 1);
                }
            }
            size_t text_len = strlen(text);
            while (text_len > 0 && (text[text_len - 1] == '\n' || text[text_len - 1] == '\r')) {
                text_len--;
            }
            if (text_len > 0) {
                char prefix[32];
                time_prefix(prefix, sizeof(prefix));
                size_t p = strlen(prefix);
                p += snprintf(prefix + p, sizeof(prefix) - p, " #%d ", s_turn);
                ring_put(prefix, p);
                ring_put(text, text_len);
                ring_put("\n", 1);
                s_lines++;
            }
        }
        xSemaphoreGive(s_lock);
    }
    va_end(copy);
    return ret;
}

void debug_trace_set(bool on)
{
    if (on && !s_ring) {
        s_lock = xSemaphoreCreateMutex();
        s_line = heap_caps_malloc(TRACE_LINE_MAX, MALLOC_CAP_SPIRAM);
        s_ring = heap_caps_malloc(TRACE_RING_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_lock || !s_line || !s_ring) {
            ESP_LOGE(TAG, "No memory for the traces");
            free(s_ring);
            free(s_line);
            s_ring = NULL;
            s_line = NULL;
            return;
        }
        s_prev = esp_log_set_vprintf(trace_vprintf);
    }
    if (on == s_on) {
        return;
    }
    s_on = on;
    // Said in the traces too (on: the first line, off: the last)
    ESP_LOGI(TAG, "Debug traces %s (firmware %s)", on ? "on" : "off", esp_app_get_description()->version);
}

bool debug_trace_on(void)
{
    return s_on;
}

void debug_trace_turn(const char *how)
{
    if (!s_on) {
        return;
    }
    s_turn++;
    wifi_ap_record_t ap;
    int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    ESP_LOGI(TAG, "===== exchange %d (%s): Wi-Fi %d dBm, internal RAM %u free (largest %u), PSRAM %u free, "
             "volume %d", s_turn, how, rssi, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), app_volume_get());
}

int debug_trace_mark(const char *how)
{
    int mark = ++s_marks;
    ESP_LOGW(TAG, "*************** MARK %d (%s) ***************", mark, how);
    return mark;
}

void debug_trace_stats(size_t *lines, size_t *bytes)
{
    *lines = s_lines;
    *bytes = s_ring ? (s_wrapped ? TRACE_RING_SIZE : s_pos) : 0;
}

bool debug_trace_dump(void)
{
    if (!s_ring) {
        return false;
    }
    ESP_LOGI(TAG, "Dump of the traces (%u lines written)", (unsigned)s_lines);
    // A copy in order, the oldest first (the traces go on meanwhile)
    char *copy = heap_caps_malloc(TRACE_RING_SIZE, MALLOC_CAP_SPIRAM);
    if (!copy) {
        ESP_LOGE(TAG, "No memory for the dump");
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t len = 0;
    if (s_wrapped) {
        memcpy(copy, s_ring + s_pos, TRACE_RING_SIZE - s_pos);
        len = TRACE_RING_SIZE - s_pos;
    }
    memcpy(copy + len, s_ring, s_pos);
    len += s_pos;
    xSemaphoreGive(s_lock);

    // The file of the last dump replaced; at most what SPIFFS has room for, the newest lines
    remove(DEBUG_TRACE_FILE);
    size_t total = 0, used = 0;
    size_t room = len;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        room = total > used + TRACE_SPIFFS_SPARE ? total - used - TRACE_SPIFFS_SPARE : 0;
    }
    size_t from = 0;
    if (len > room) {
        from = len - room;
    }
    if (s_wrapped || from > 0) {
        // Starting at a whole line
        while (from < len && copy[from] != '\n') {
            from++;
        }
        from += from < len;
    }
    // Written through a buffer in internal RAM, a piece at a time: a single write of the whole PSRAM copy stopped
    // after the first 128 bytes
    char *piece = heap_caps_malloc(TRACE_DUMP_PIECE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    FILE *f = piece ? fopen(DEBUG_TRACE_FILE, "w") : NULL;
    bool ok = f != NULL;
    size_t written = 0;
    if (f) {
        char now[32];
        time_prefix(now, sizeof(now));
        int n = snprintf(piece, TRACE_DUMP_PIECE,
                         "# DEBUG.TXT: traces of firmware %s, dumped at %s (%u lines written since the traces "
                         "started%s)\n", esp_app_get_description()->version, now, (unsigned)s_lines,
                         s_wrapped || from > 0 ? ", the oldest dropped" : "");
        ok = fwrite(piece, 1, n, f) == (size_t)n;
        while (ok && from + written < len) {
            size_t chunk = len - from - written < TRACE_DUMP_PIECE ? len - from - written : TRACE_DUMP_PIECE;
            memcpy(piece, copy + from + written, chunk);
            ok = fwrite(piece, 1, chunk, f) == chunk;
            written += ok ? chunk : 0;
        }
        if (!ok) {
            ESP_LOGE(TAG, "Dump stopped after %u bytes: errno %d (SPIFFS %u of %u bytes used)", (unsigned)written,
                     errno, (unsigned)used, (unsigned)total);
        }
        ok &= fclose(f) == 0;
    }
    free(piece);
    free(copy);
    ESP_LOGI(TAG, "%s: %u of %u bytes written%s", DEBUG_TRACE_FILE, (unsigned)written, (unsigned)(len - from),
             ok ? "" : ", error");
    return ok;
}
