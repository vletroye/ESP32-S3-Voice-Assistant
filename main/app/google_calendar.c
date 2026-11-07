/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Google Calendar for the model: see google_calendar.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "google_auth.h"
#include "google_calendar.h"

static const char *TAG = "gcal";

#define CALENDAR_API        "https://www.googleapis.com/calendar/v3/calendars/primary/events"
#define CALENDAR_DAYS_MAX   31
#define CALENDAR_EVENTS_MAX 50
#define SERVICE             "Google Calendar"

static const char s_definitions[] =
    "["
    "{\"name\":\"calendar_events\","
    "\"description\":\"The events of the user's Google Calendar (main calendar) over a period: id, title, start and "
    "end (local time, or all_day), place. Use it for any question about the agenda: what is planned, whether a slot "
    "is free, when the next appointment is, and to find the id of an event to change. Say them briefly in time "
    "order; never read ids.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"from\":{\"type\":\"string\",\"description\":\"First day, YYYY-MM-DD (local); today when left out\"},"
    "\"days\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":31,\"description\":\"Number of days from it, 1 by "
    "default\"},"
    "\"query\":{\"type\":\"string\",\"description\":\"Words of the events looked for (e.g. 'dentiste'), to find "
    "one\"}}}},"
    "{\"name\":\"calendar_create\","
    "\"description\":\"Create an event in the user's Google Calendar. Confirm in a few words with the day and "
    "time.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"title\":{\"type\":\"string\",\"description\":\"In the user's language\"},"
    "\"start\":{\"type\":\"string\",\"description\":\"Local start, YYYY-MM-DDTHH:MM; or YYYY-MM-DD for an all-day "
    "event\"},"
    "\"end\":{\"type\":\"string\",\"description\":\"Local end, same form; when left out: one hour later (the next "
    "day for an all-day one)\"},"
    "\"place\":{\"type\":\"string\"},"
    "\"notes\":{\"type\":\"string\"}},"
    "\"required\":[\"title\",\"start\"]}},"
    "{\"name\":\"calendar_update\","
    "\"description\":\"Move or change an event of the user's Google Calendar, by its id (from calendar_events). "
    "Without confirmed=true nothing is changed: the event is given back; tell the user what will change and call "
    "again with confirmed=true only once they agree.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"id\":{\"type\":\"string\"},"
    "\"title\":{\"type\":\"string\"},"
    "\"start\":{\"type\":\"string\",\"description\":\"New local start, YYYY-MM-DDTHH:MM (or YYYY-MM-DD)\"},"
    "\"end\":{\"type\":\"string\",\"description\":\"New local end; when the start moves without it, the "
    "duration is kept\"},"
    "\"place\":{\"type\":\"string\"},"
    "\"confirmed\":{\"type\":\"boolean\"}},"
    "\"required\":[\"id\"]}},"
    "{\"name\":\"calendar_delete\","
    "\"description\":\"Delete an event of the user's Google Calendar, by its id (from calendar_events). Without "
    "confirmed=true nothing is deleted: the event is given back; ask the user to confirm, then call again with "
    "confirmed=true.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"id\":{\"type\":\"string\"},"
    "\"confirmed\":{\"type\":\"boolean\"}},"
    "\"required\":[\"id\"]}}"
    "]";

const char *google_calendar_definitions(void)
{
    return google_auth_state() == GOOGLE_NOT_CONFIGURED ? NULL : s_definitions;
}

bool google_calendar_has(const char *name)
{
    return strcmp(name, "calendar_events") == 0 || strcmp(name, "calendar_create") == 0 ||
           strcmp(name, "calendar_update") == 0 || strcmp(name, "calendar_delete") == 0;
}

/* ---------- times ---------- */

/* Days since 1970-01-01 of a date (proleptic Gregorian) */
static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* "2026-11-10T15:00:00+01:00", "...Z", "...:00.000Z" -> the time; 0 when not understood */
static time_t rfc3339_parse(const char *text)
{
    int y, mo, d, h, mi, s = 0;
    if (!text || sscanf(text, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 5) {
        return 0;
    }
    long offset = 0;
    // The zone after the time (the date's dashes are before it)
    const char *z = strlen(text) > 16 ? strpbrk(text + 16, "Z+-") : NULL;
    if (z && (*z == '+' || *z == '-')) {
        int oh = 0, om = 0;
        sscanf(z + 1, "%d:%d", &oh, &om);
        offset = (oh * 3600L + om * 60L) * (*z == '-' ? -1 : 1);
    }
    return (time_t)(days_from_civil(y, mo, d) * 86400L + h * 3600L + mi * 60L + s - offset);
}

/* "2026-11-10T15:00" or "2026-11-10" (local) -> the time; *all_day for a date alone; 0 when not understood */
static time_t local_parse(const char *text, bool *all_day)
{
    struct tm t = {0};
    *all_day = false;
    if (!text) {
        return 0;
    }
    int n = sscanf(text, "%d-%d-%dT%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min);
    if (n == 3) {
        *all_day = true;
    } else if (n != 5) {
        return 0;
    }
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    t.tm_isdst = -1;
    return mktime(&t);
}

/* The time in UTC for the API: "2026-11-10T14:00:00Z" */
static void utc_text(time_t when, char *out, size_t size)
{
    struct tm t;
    gmtime_r(&when, &t);
    strftime(out, size, "%Y-%m-%dT%H:%M:%SZ", &t);
}

/* The local time for the model: "2026-11-10 15:00" */
static void local_text(time_t when, char *out, size_t size)
{
    struct tm t;
    localtime_r(&when, &t);
    strftime(out, size, "%Y-%m-%d %H:%M", &t);
}

static const char *const s_days[] = { "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday" };

/* ---------- events ---------- */

static const char *string_input(const cJSON *input, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(input, key);
    return cJSON_IsString(item) && item->valuestring[0] ? item->valuestring : NULL;
}

static char *error_json(const char *message)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", message);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return text;
}

/* An event of the API made short for the model: id, title, start, end (or all_day), weekday, place */
static cJSON *event_short(const cJSON *item)
{
    cJSON *e = cJSON_CreateObject();
    const char *id = string_input(item, "id");
    cJSON_AddStringToObject(e, "id", id ? id : "");
    const char *title = string_input(item, "summary");
    cJSON_AddStringToObject(e, "title", title ? title : "");
    const cJSON *start = cJSON_GetObjectItem(item, "start");
    const cJSON *end = cJSON_GetObjectItem(item, "end");
    const char *start_date = string_input(start, "date");
    char text[24];
    if (start_date) {
        // All day: Google's end date is the day after the last one
        cJSON_AddStringToObject(e, "date", start_date);
        cJSON_AddBoolToObject(e, "all_day", true);
        const char *end_date = string_input(end, "date");
        if (end_date) {
            cJSON_AddStringToObject(e, "end_date_excluded", end_date);
        }
        struct tm t = {0};
        if (sscanf(start_date, "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday) == 3) {
            t.tm_year -= 1900;
            t.tm_mon -= 1;
            t.tm_hour = 12;
            t.tm_isdst = -1;
            mktime(&t);
            cJSON_AddStringToObject(e, "weekday", s_days[t.tm_wday]);
        }
    } else {
        time_t from = rfc3339_parse(string_input(start, "dateTime"));
        time_t to = rfc3339_parse(string_input(end, "dateTime"));
        local_text(from, text, sizeof(text));
        cJSON_AddStringToObject(e, "start", text);
        local_text(to, text, sizeof(text));
        cJSON_AddStringToObject(e, "end", text);
        struct tm t;
        localtime_r(&from, &t);
        cJSON_AddStringToObject(e, "weekday", s_days[t.tm_wday]);
    }
    const char *place = string_input(item, "location");
    if (place) {
        cJSON_AddStringToObject(e, "place", place);
    }
    return e;
}

/* "a b" -> "a%20b" */
static void url_encode(char *out, size_t size, const char *in)
{
    size_t n = 0;
    for (const unsigned char *c = (const unsigned char *)in; *c && n + 4 < size; c++) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr("-_.~", *c)) {
            out[n++] = (char)*c;
        } else {
            n += snprintf(out + n, size - n, "%%%02X", *c);
        }
    }
    out[n] = '\0';
}

static char *run_events(const cJSON *input)
{
    bool all_day;
    const char *from_text = string_input(input, "from");
    time_t from = from_text ? local_parse(from_text, &all_day) : time(NULL);
    if (!from) {
        return error_json("give from as YYYY-MM-DD");
    }
    struct tm t;
    localtime_r(&from, &t);
    t.tm_hour = t.tm_min = t.tm_sec = 0;
    t.tm_isdst = -1;
    from = mktime(&t);
    const cJSON *days = cJSON_GetObjectItem(input, "days");
    int n = cJSON_IsNumber(days) ? days->valueint : 1;
    n = n < 1 ? 1 : n > CALENDAR_DAYS_MAX ? CALENDAR_DAYS_MAX : n;
    t.tm_mday += n;
    t.tm_isdst = -1;
    time_t to = mktime(&t);
    char min[24], max[24];
    utc_text(from, min, sizeof(min));
    utc_text(to, max, sizeof(max));
    static char url[700];
    int len = snprintf(url, sizeof(url), CALENDAR_API "?timeMin=%s&timeMax=%s&singleEvents=true&orderBy=startTime"
                       "&maxResults=%d&fields=items(id,summary,location,start,end)", min, max, CALENDAR_EVENTS_MAX);
    const char *query = string_input(input, "query");
    if (query) {
        char q[200];
        url_encode(q, sizeof(q), query);
        snprintf(url + len, sizeof(url) - len, "&q=%s", q);
    }
    int status;
    char *response = google_api("GET", url, NULL, &status);
    cJSON *json = status == 200 && response ? cJSON_Parse(response) : NULL;
    if (!json) {
        char *error = google_api_error(status, response, SERVICE);
        free(response);
        return error;
    }
    free(response);
    cJSON *o = cJSON_CreateObject();
    char day[16];
    strftime(day, sizeof(day), "%Y-%m-%d", localtime_r(&from, &t));
    cJSON_AddStringToObject(o, "from", day);
    cJSON_AddNumberToObject(o, "days", n);
    cJSON *events = cJSON_AddArrayToObject(o, "events");
    const cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(json, "items")) {
        cJSON_AddItemToArray(events, event_short(item));
    }
    cJSON_Delete(json);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return text;
}

/* start / end of the API from local times: {"dateTime":"...Z"} or {"date":"YYYY-MM-DD"} */
static void when_add(cJSON *body, const char *key, time_t when, bool all_day)
{
    cJSON *o = cJSON_AddObjectToObject(body, key);
    char text[24];
    if (all_day) {
        struct tm t;
        localtime_r(&when, &t);
        strftime(text, sizeof(text), "%Y-%m-%d", &t);
        cJSON_AddStringToObject(o, "date", text);
    } else {
        utc_text(when, text, sizeof(text));
        cJSON_AddStringToObject(o, "dateTime", text);
    }
}

/* The event (allocated JSON of the API), NULL with *error */
static cJSON *event_get(const char *id, char **error)
{
    char url[320];
    snprintf(url, sizeof(url), CALENDAR_API "/%s", id);
    int status;
    char *response = google_api("GET", url, NULL, &status);
    cJSON *json = status == 200 && response ? cJSON_Parse(response) : NULL;
    if (!json) {
        *error = status == 404 ? error_json("no event with this id: call calendar_events") :
                 google_api_error(status, response, SERVICE);
    }
    free(response);
    return json;
}

/* The answer of a change made: what it is now */
static char *event_answer(const char *done, const char *response)
{
    cJSON *json = response ? cJSON_Parse(response) : NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "done", done);
    if (json) {
        cJSON_AddItemToObject(o, "event", event_short(json));
    }
    cJSON_Delete(json);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return text;
}

static char *run_create(const cJSON *input, bool *is_error)
{
    const char *title = string_input(input, "title");
    bool all_day, end_all_day;
    time_t start = local_parse(string_input(input, "start"), &all_day);
    if (!title || !start) {
        *is_error = true;
        return error_json("give title, and start as YYYY-MM-DDTHH:MM or YYYY-MM-DD");
    }
    time_t end = local_parse(string_input(input, "end"), &end_all_day);
    if (!end || end <= start) {
        end = start + (all_day ? 86400 : 3600);
    } else if (all_day) {
        end += 86400;       // the day after the last one, as Google wants it
    }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "summary", title);
    when_add(body, "start", start, all_day);
    when_add(body, "end", end, all_day);
    if (string_input(input, "place")) {
        cJSON_AddStringToObject(body, "location", string_input(input, "place"));
    }
    if (string_input(input, "notes")) {
        cJSON_AddStringToObject(body, "description", string_input(input, "notes"));
    }
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    int status;
    char *response = google_api("POST", CALENDAR_API, text, &status);
    free(text);
    char *answer;
    if (status == 200) {
        answer = event_answer("created", response);
        ESP_LOGI(TAG, "event created");
    } else {
        *is_error = true;
        answer = google_api_error(status, response, SERVICE);
    }
    free(response);
    return answer;
}

/* Not confirmed yet: the event, and what to do */
static char *confirmation(const cJSON *event, const char *action)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "confirmation_required", true);
    cJSON_AddStringToObject(o, "action", action);
    cJSON_AddItemToObject(o, "event", event_short(event));
    cJSON_AddStringToObject(o, "instruction", "Nothing was changed. Tell the user briefly what will change and ask "
                            "them to confirm; call again with confirmed=true only if they agree.");
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return text;
}

static char *run_update(const cJSON *input, bool *is_error)
{
    const char *id = string_input(input, "id");
    if (!id) {
        *is_error = true;
        return error_json("give id (from calendar_events)");
    }
    char *error = NULL;
    cJSON *event = event_get(id, &error);
    if (!event) {
        *is_error = true;
        return error;
    }
    if (!cJSON_IsTrue(cJSON_GetObjectItem(input, "confirmed"))) {
        char *answer = confirmation(event, "update");
        cJSON_Delete(event);
        return answer;
    }
    // Only what changes is sent (PATCH); a start moved without an end keeps the duration
    cJSON *body = cJSON_CreateObject();
    if (string_input(input, "title")) {
        cJSON_AddStringToObject(body, "summary", string_input(input, "title"));
    }
    if (string_input(input, "place")) {
        cJSON_AddStringToObject(body, "location", string_input(input, "place"));
    }
    bool all_day, end_all_day;
    time_t start = local_parse(string_input(input, "start"), &all_day);
    time_t end = local_parse(string_input(input, "end"), &end_all_day);
    if (start) {
        if (!end) {
            const cJSON *s = cJSON_GetObjectItem(event, "start");
            const cJSON *e = cJSON_GetObjectItem(event, "end");
            time_t old_start = rfc3339_parse(string_input(s, "dateTime"));
            time_t old_end = rfc3339_parse(string_input(e, "dateTime"));
            end = start + (old_start && old_end > old_start ? old_end - old_start : all_day ? 86400 : 3600);
        } else if (all_day) {
            end += 86400;
        }
        when_add(body, "start", start, all_day);
        when_add(body, "end", end, all_day);
    }
    cJSON_Delete(event);
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    char url[320];
    snprintf(url, sizeof(url), CALENDAR_API "/%s", id);
    int status;
    char *response = google_api("PATCH", url, text, &status);
    free(text);
    char *answer;
    if (status == 200) {
        answer = event_answer("updated", response);
        ESP_LOGI(TAG, "event updated");
    } else {
        *is_error = true;
        answer = google_api_error(status, response, SERVICE);
    }
    free(response);
    return answer;
}

static char *run_delete(const cJSON *input, bool *is_error)
{
    const char *id = string_input(input, "id");
    if (!id) {
        *is_error = true;
        return error_json("give id (from calendar_events)");
    }
    char *error = NULL;
    cJSON *event = event_get(id, &error);
    if (!event) {
        *is_error = true;
        return error;
    }
    if (!cJSON_IsTrue(cJSON_GetObjectItem(input, "confirmed"))) {
        char *answer = confirmation(event, "delete");
        cJSON_Delete(event);
        return answer;
    }
    cJSON *gone = event_short(event);
    cJSON_Delete(event);
    char url[320];
    snprintf(url, sizeof(url), CALENDAR_API "/%s", id);
    int status;
    char *response = google_api("DELETE", url, NULL, &status);
    char *answer;
    if (status == 200 || status == 204) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "done", "deleted");
        cJSON_AddItemToObject(o, "event", gone);
        answer = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        ESP_LOGI(TAG, "event deleted");
    } else {
        cJSON_Delete(gone);
        *is_error = true;
        answer = google_api_error(status, response, SERVICE);
    }
    free(response);
    return answer;
}

char *google_calendar_run(const char *name, const cJSON *input, bool *is_error)
{
    *is_error = false;
    char *result;
    if (strcmp(name, "calendar_events") == 0) {
        result = run_events(input);
    } else if (strcmp(name, "calendar_create") == 0) {
        result = run_create(input, is_error);
    } else if (strcmp(name, "calendar_update") == 0) {
        result = run_update(input, is_error);
    } else {
        result = run_delete(input, is_error);
    }
    if (result && strncmp(result, "{\"error\"", 8) == 0) {
        *is_error = true;
    }
    return result;
}
