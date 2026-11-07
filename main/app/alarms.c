/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "file_state.h"
#include "alarms.h"

#define ALARMS_FILE         STATE_DIR "/alarms.json"
#define ALARMS_TMP_FILE     STATE_DIR "/alarms.tmp"
#define ALARMS_MAX          50
/* Due more than this before the clock was set at start-up (the device was off): missed, not rung late. One due
   since (rung once the assistant sleeps, after a conversation) rings however late. */
#define ALARM_LATE_S        120
/* A missed one-time alarm or reminder older than this is dropped without a word */
#define ALARM_MISSED_KEEP_S (12 * 3600)
/* The clock is set once it is past this (1 January 2024) */
#define CLOCK_SET_AFTER     1704067200
/* Days looked through for the next time of a repeated one (a 29 February: up to 4 years) */
#define NEXT_DAYS_MAX       (4 * 366 + 1)

static const char *TAG = "alarms";

static alarm_t *s_alarms = NULL;    /* PSRAM, ALARMS_MAX */
static int s_count = 0;
static int s_next_id = 1;
static SemaphoreHandle_t s_lock = NULL;

static const char *const s_kind_names[] = { "timer", "alarm", "reminder" };
static const char *const s_repeat_names[] = {
    "none", "daily", "weekdays", "weekend", "weekly", "monthly", "monthly_nth_weekday", "monthly_last_weekday",
    "monthly_last_day", "yearly",
};
static const char *const s_day_names[] = { "sunday", "monday", "tuesday", "wednesday", "thursday", "friday",
                                           "saturday" };

bool alarms_clock_set(void)
{
    return time(NULL) > CLOCK_SET_AFTER;
}

alarm_repeat_t alarms_repeat_from(const char *name)
{
    for (size_t i = 0; name && i < sizeof(s_repeat_names) / sizeof(s_repeat_names[0]); i++) {
        if (strcmp(name, s_repeat_names[i]) == 0) {
            return (alarm_repeat_t)i;
        }
    }
    return ALARM_REPEAT_NONE;
}

/* ---------- The next time of a repeated one ---------- */

static int days_in_month(int year, int month)
{
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int y = year + 1900;
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return month == 1 && leap ? 29 : days[month];
}

/* The day (its tm normalised by mktime) is one of the alarm's */
static bool day_matches(const alarm_t *a, const struct tm *d)
{
    int dim = days_in_month(d->tm_year, d->tm_mon);
    int month_day = a->month_day < dim ? a->month_day : dim;
    switch (a->repeat) {
    case ALARM_REPEAT_DAILY:
        return true;
    case ALARM_REPEAT_WEEKDAYS:
        return d->tm_wday >= 1 && d->tm_wday <= 5;
    case ALARM_REPEAT_WEEKEND:
        return d->tm_wday == 0 || d->tm_wday == 6;
    case ALARM_REPEAT_WEEKLY:
        return (a->days >> d->tm_wday) & 1;
    case ALARM_REPEAT_MONTHLY:
        return d->tm_mday == month_day;
    case ALARM_REPEAT_MONTHLY_NTH:
        return d->tm_wday == a->week_day && (d->tm_mday - 1) / 7 + 1 == a->nth;
    case ALARM_REPEAT_MONTHLY_LAST:
        return d->tm_wday == a->week_day && d->tm_mday + 7 > dim;
    case ALARM_REPEAT_MONTHLY_END:
        return d->tm_mday == dim;
    case ALARM_REPEAT_YEARLY:
        return d->tm_mon == a->month && d->tm_mday == month_day;
    default:
        return false;
    }
}

/* Its first time after `after` (local time), 0 if none */
static time_t next_time(const alarm_t *a, time_t after)
{
    struct tm day;
    localtime_r(&after, &day);
    for (int i = 0; i < NEXT_DAYS_MAX; i++) {
        struct tm t = day;
        t.tm_mday += i;
        t.tm_hour = a->hour;
        t.tm_min = a->minute;
        t.tm_sec = 0;
        t.tm_isdst = -1;
        time_t when = mktime(&t);     // normalises t (the day after the 31st...)
        if (when > after && day_matches(a, &t)) {
            return when;
        }
    }
    return 0;
}

/* ---------- The file ---------- */

static void format_local(time_t when, char *out, size_t size)
{
    struct tm t;
    localtime_r(&when, &t);
    strftime(out, size, "%Y-%m-%dT%H:%M", &t);
}

/* Saved in full each time (a few KB at most), through a temporary file */
static void save(void)
{
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < s_count; i++) {
        const alarm_t *a = &s_alarms[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", a->id);
        cJSON_AddNumberToObject(o, "kind", a->kind);
        cJSON_AddStringToObject(o, "label", a->label);
        cJSON_AddNumberToObject(o, "due", (double)a->due);
        cJSON_AddNumberToObject(o, "repeat", a->repeat);
        cJSON_AddNumberToObject(o, "days", a->days);
        cJSON_AddNumberToObject(o, "hour", a->hour);
        cJSON_AddNumberToObject(o, "minute", a->minute);
        cJSON_AddNumberToObject(o, "month_day", a->month_day);
        cJSON_AddNumberToObject(o, "month", a->month);
        cJSON_AddNumberToObject(o, "week_day", a->week_day);
        cJSON_AddNumberToObject(o, "nth", a->nth);
        cJSON_AddStringToObject(o, "radio", a->radio);
        cJSON_AddNumberToObject(o, "seconds", a->seconds);
        cJSON_AddItemToArray(list, o);
    }
    char *text = cJSON_PrintUnformatted(list);
    cJSON_Delete(list);
    FILE *f = text ? fopen(ALARMS_TMP_FILE, "w") : NULL;
    bool ok = f && fputs(text, f) >= 0;
    if (f) {
        ok &= fclose(f) == 0;
    }
    free(text);
    if (ok && rename(ALARMS_TMP_FILE, ALARMS_FILE) != 0) {
        remove(ALARMS_FILE);
        ok = rename(ALARMS_TMP_FILE, ALARMS_FILE) == 0;
    }
    if (!ok) {
        ESP_LOGE(TAG, "%s not saved", ALARMS_FILE);
    }
}

static int json_int(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}

void alarms_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_alarms = heap_caps_calloc(ALARMS_MAX, sizeof(alarm_t), MALLOC_CAP_SPIRAM);
    FILE *f = fopen(ALARMS_FILE, "r");
    if (!f || !s_alarms) {
        if (f) {
            fclose(f);
        }
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = size > 0 ? malloc(size + 1) : NULL;
    size_t n = text ? fread(text, 1, size, f) : 0;
    fclose(f);
    if (!text) {
        return;
    }
    text[n] = '\0';
    cJSON *list = cJSON_Parse(text);
    free(text);
    const cJSON *o;
    cJSON_ArrayForEach(o, list) {
        if (s_count >= ALARMS_MAX) {
            break;
        }
        alarm_t *a = &s_alarms[s_count++];
        a->id = json_int(o, "id");
        a->kind = (alarm_kind_t)json_int(o, "kind");
        const cJSON *label = cJSON_GetObjectItem(o, "label");
        strlcpy(a->label, cJSON_IsString(label) ? label->valuestring : "", sizeof(a->label));
        const cJSON *due = cJSON_GetObjectItem(o, "due");
        a->due = cJSON_IsNumber(due) ? (time_t)due->valuedouble : 0;
        a->repeat = (alarm_repeat_t)json_int(o, "repeat");
        a->days = (unsigned)json_int(o, "days");
        a->hour = json_int(o, "hour");
        a->minute = json_int(o, "minute");
        a->month_day = json_int(o, "month_day");
        a->month = json_int(o, "month");
        a->week_day = json_int(o, "week_day");
        a->nth = json_int(o, "nth");
        const cJSON *radio = cJSON_GetObjectItem(o, "radio");
        strlcpy(a->radio, cJSON_IsString(radio) ? radio->valuestring : "", sizeof(a->radio));
        a->seconds = json_int(o, "seconds");
        if (a->id >= s_next_id) {
            s_next_id = a->id + 1;
        }
    }
    cJSON_Delete(list);
    ESP_LOGI(TAG, "%d timer(s), alarm(s) and reminder(s) kept", s_count);
}

/* ---------- The list ---------- */

static void remove_at(int i)
{
    memmove(&s_alarms[i], &s_alarms[i + 1], (s_count - i - 1) * sizeof(alarm_t));
    s_count--;
}

bool alarms_add(alarm_t *alarm)
{
    if (!s_alarms) {
        return false;
    }
    time_t now = time(NULL);
    if (alarm->repeat != ALARM_REPEAT_NONE) {
        // The time of day and the day it refers to, from its first time (the model gives a date and a time)
        struct tm t;
        localtime_r(&alarm->due, &t);
        alarm->hour = t.tm_hour;
        alarm->minute = t.tm_min;
        alarm->month_day = t.tm_mday;
        alarm->month = t.tm_mon;
        alarm->week_day = t.tm_wday;
        alarm->nth = (t.tm_mday - 1) / 7 + 1;
        if (alarm->repeat == ALARM_REPEAT_WEEKLY && !alarm->days) {
            alarm->days = 1u << t.tm_wday;
        }
        time_t from = alarm->due - 1 > now ? alarm->due - 1 : now;
        alarm->due = next_time(alarm, from);
    }
    if (alarm->due <= now) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_count < ALARMS_MAX;
    if (ok) {
        alarm->id = s_next_id++;
        s_alarms[s_count++] = *alarm;
        save();
    }
    xSemaphoreGive(s_lock);
    if (ok) {
        char when[20];
        format_local(alarm->due, when, sizeof(when));
        ESP_LOGI(TAG, "%s %d \"%s\" at %s (%s)", s_kind_names[alarm->kind], alarm->id, alarm->label, when,
                 s_repeat_names[alarm->repeat]);
    }
    return ok;
}

bool alarms_remove(int id)
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count && !found; i++) {
        if (s_alarms[i].id == id) {
            remove_at(i);
            found = true;
        }
    }
    if (found) {
        save();
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%d %s", id, found ? "removed" : "not found");
    return found;
}

int alarms_remove_all(int kind)
{
    int removed = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = s_count - 1; i >= 0; i--) {
        if (kind < 0 || s_alarms[i].kind == (alarm_kind_t)kind) {
            remove_at(i);
            removed++;
        }
    }
    if (removed) {
        save();
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%d removed", removed);
    return removed;
}

static int compare_due(const void *a, const void *b)
{
    time_t x = ((const alarm_t *)a)->due, y = ((const alarm_t *)b)->due;
    return x < y ? -1 : x > y;
}

alarm_t *alarms_list(int *count)
{
    *count = 0;
    if (!s_lock) {
        return NULL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    alarm_t *copy = s_count ? heap_caps_malloc(s_count * sizeof(alarm_t), MALLOC_CAP_SPIRAM) : NULL;
    if (copy) {
        memcpy(copy, s_alarms, s_count * sizeof(alarm_t));
        *count = s_count;
    }
    xSemaphoreGive(s_lock);
    if (copy) {
        qsort(copy, *count, sizeof(alarm_t), compare_due);
    }
    return copy;
}

bool alarms_next(int kind, alarm_t *out)
{
    bool found = false;
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        const alarm_t *a = &s_alarms[i];
        if ((kind < 0 || a->kind == (alarm_kind_t)kind) && (!found || a->due < out->due)) {
            *out = *a;
            found = true;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

bool alarms_due(alarm_t *out, bool *missed)
{
    *missed = false;
    if (!s_lock || !s_count || !alarms_clock_set()) {
        return false;
    }
    time_t now = time(NULL);
    static time_t clock_seen = 0;       // the first time the clock was set, since the start
    if (!clock_seen) {
        clock_seen = now;
    }
    bool found = false;
    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    while (!found) {
        int first = -1;
        for (int i = 0; i < s_count; i++) {
            if (s_alarms[i].due <= now && (first < 0 || s_alarms[i].due < s_alarms[first].due)) {
                first = i;
            }
        }
        if (first < 0) {
            break;
        }
        alarm_t a = s_alarms[first];
        bool late = a.due < clock_seen - ALARM_LATE_S;
        changed = true;
        if (a.repeat == ALARM_REPEAT_NONE) {
            remove_at(first);
        } else {
            s_alarms[first].due = next_time(&a, now);
            if (!s_alarms[first].due) {
                remove_at(first);
            }
        }
        if (!late) {
            *out = a;
            found = true;
        } else if (a.repeat == ALARM_REPEAT_NONE && now - a.due <= ALARM_MISSED_KEEP_S) {
            // Missed (the device was off): said, not rung
            *out = a;
            *missed = true;
            found = true;
        } else {
            ESP_LOGW(TAG, "%s %d missed (%lld min late): %s", s_kind_names[a.kind], a.id,
                     (long long)(now - a.due) / 60, a.repeat == ALARM_REPEAT_NONE ? "dropped" : "next time kept");
        }
    }
    if (changed) {
        save();
    }
    xSemaphoreGive(s_lock);
    if (found) {
        ESP_LOGI(TAG, "%s %d \"%s\" %s", s_kind_names[out->kind], out->id, out->label, *missed ? "missed" : "due");
    }
    return found;
}

bool alarms_snooze(const alarm_t *alarm, int minutes)
{
    alarm_t again = *alarm;
    again.repeat = ALARM_REPEAT_NONE;
    again.due = time(NULL) + minutes * 60;
    return alarms_add(&again);
}

cJSON *alarms_to_json(const alarm_t *a)
{
    cJSON *o = cJSON_CreateObject();
    char when[20];
    format_local(a->due, when, sizeof(when));
    struct tm t;
    localtime_r(&a->due, &t);
    cJSON_AddNumberToObject(o, "id", a->id);
    cJSON_AddStringToObject(o, "kind", s_kind_names[a->kind]);
    if (a->label[0]) {
        cJSON_AddStringToObject(o, "label", a->label);
    }
    cJSON_AddStringToObject(o, "when", when);
    cJSON_AddStringToObject(o, "weekday", s_day_names[t.tm_wday]);
    if (a->kind == ALARM_KIND_TIMER) {
        long left = (long)(a->due - time(NULL));
        cJSON_AddNumberToObject(o, "seconds_left", left > 0 ? left : 0);
    }
    if (a->repeat != ALARM_REPEAT_NONE) {
        cJSON_AddStringToObject(o, "repeat", s_repeat_names[a->repeat]);
    }
    if (a->repeat == ALARM_REPEAT_WEEKLY) {
        cJSON *days = cJSON_AddArrayToObject(o, "days");
        for (int d = 0; d < 7; d++) {
            if ((a->days >> d) & 1) {
                cJSON_AddItemToArray(days, cJSON_CreateString(s_day_names[d]));
            }
        }
    }
    if (a->radio[0]) {
        cJSON_AddStringToObject(o, "radio", a->radio);
    }
    return o;
}

/* For the tools: a day's name ("monday") to its bit */
int alarms_day_bit(const char *name)
{
    for (int d = 0; d < 7; d++) {
        if (name && strncmp(name, s_day_names[d], 3) == 0) {
            return 1 << d;
        }
    }
    return 0;
}
