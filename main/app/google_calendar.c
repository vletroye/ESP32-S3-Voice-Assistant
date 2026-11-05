/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Google Calendar, read only: see google_calendar.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "file_state.h"
#include "google_calendar.h"
#include "gcal_client.h"        /* GCAL_CLIENT_ID, GCAL_CLIENT_SECRET (build directory) */

static const char *TAG = "gcal";

#define GCAL_TOKEN_FILE     STATE_DIR "/gcal.json"
#define GCAL_DEVICE_URL     "https://oauth2.googleapis.com/device/code"
#define GCAL_TOKEN_URL      "https://oauth2.googleapis.com/token"
#define GCAL_REVOKE_URL     "https://oauth2.googleapis.com/revoke"
#define GCAL_EVENTS_URL     "https://www.googleapis.com/calendar/v3/calendars/primary/events"
#define GCAL_HTTP_TIMEOUT   10000
#define GCAL_RESPONSE_MAX   (64 * 1024)
#define GCAL_EVENTS_MAX     25
#define GCAL_DAYS_MAX       7

static char *s_refresh = NULL;              /* the refresh token, kept in GCAL_TOKEN_FILE */
static char *s_access = NULL;               /* the access token, valid until s_access_until */
static int64_t s_access_until = 0;
static SemaphoreHandle_t s_lock = NULL;     /* the tokens (main task: linking; chat task: the tool) */

/* Linking (main task) */
static volatile gcal_link_state_t s_state = GCAL_LINK_IDLE;
static volatile bool s_start = false;
static char s_device_code[200];
static char s_user_code[24];
static char s_url[64];
static int64_t s_expires_us = 0;
static int64_t s_next_poll_us = 0;
static int s_interval_s = 5;
static volatile bool s_unlink = false;
static void unlink_now(void);

bool gcal_configured(void)
{
    return GCAL_CLIENT_ID[0] != '\0';
}

bool gcal_linked(void)
{
    return s_refresh != NULL && !s_unlink;
}

/* "a b&c" -> "a%20b%26c": a value of a form or of a query */
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

/*
 * An HTTPS request: POST with a form body (body not NULL) or GET, with the access token when bearer; the response
 * (allocated, PSRAM) and its status (-1: not reached)
 */
static char *http_request(const char *url, const char *body, const char *bearer, int *status)
{
    *status = -1;
    esp_http_client_config_t config = {
        .url = url,
        .method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = GCAL_HTTP_TIMEOUT,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return NULL;
    }
    if (body) {
        esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    }
    char *auth = NULL;
    if (bearer) {
        asprintf(&auth, "Bearer %s", bearer);
        esp_http_client_set_header(client, "Authorization", auth ? auth : "");
    }
    char *response = NULL;
    size_t len = body ? strlen(body) : 0;
    if (esp_http_client_open(client, len) == ESP_OK && (!body || esp_http_client_write(client, body, len) == (int)len) &&
            esp_http_client_fetch_headers(client) >= 0) {
        *status = esp_http_client_get_status_code(client);
        response = heap_caps_malloc(GCAL_RESPONSE_MAX + 1, MALLOC_CAP_SPIRAM);
        size_t got = 0;
        int n;
        while (response && got < GCAL_RESPONSE_MAX &&
                (n = esp_http_client_read(client, response + got, GCAL_RESPONSE_MAX - got)) > 0) {
            got += n;
        }
        if (response) {
            response[got] = '\0';
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(auth);
    return response;
}

static const char *json_string(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItem(object, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static void token_save(void)
{
    FILE *f = fopen(GCAL_TOKEN_FILE, "w");
    if (f) {
        fprintf(f, "{\"refresh_token\":\"%s\"}", s_refresh ? s_refresh : "");
        fclose(f);
    }
}

void gcal_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    FILE *f = fopen(GCAL_TOKEN_FILE, "r");
    if (!f) {
        ESP_LOGI(TAG, "%s", gcal_configured() ? "Not linked" : "No OAuth client in this firmware: not offered");
        return;
    }
    char text[600];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    cJSON *json = cJSON_Parse(text);
    const char *token = json_string(json, "refresh_token");
    if (token && token[0]) {
        s_refresh = strdup(token);
    }
    cJSON_Delete(json);
    // (the token itself is never logged, as the keys)
    ESP_LOGI(TAG, "%s", s_refresh ? "Linked to a Google account" : "Not linked");
}

/* The tokens of a token response kept; false if it has no access token */
static bool tokens_take(const char *response)
{
    cJSON *json = cJSON_Parse(response);
    const char *access = json_string(json, "access_token");
    const char *refresh = json_string(json, "refresh_token");
    const cJSON *expires = cJSON_GetObjectItem(json, "expires_in");
    if (access) {
        free(s_access);
        s_access = strdup(access);
        // A minute before Google says, not to use one about to expire
        s_access_until = esp_timer_get_time() + ((cJSON_IsNumber(expires) ? expires->valueint : 3600) - 60) * 1000000LL;
    }
    if (refresh) {
        free(s_refresh);
        s_refresh = strdup(refresh);
        token_save();
    }
    cJSON_Delete(json);
    return access != NULL;
}

/* A valid access token (under s_lock): refreshed when it expired; NULL if not linked or Google refused */
static const char *access_token(bool *revoked)
{
    *revoked = false;
    if (!s_refresh) {
        return NULL;
    }
    if (s_access && esp_timer_get_time() < s_access_until) {
        return s_access;
    }
    char refresh[600], body[900];
    url_encode(refresh, sizeof(refresh), s_refresh);
    snprintf(body, sizeof(body), "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token",
             GCAL_CLIENT_ID, GCAL_CLIENT_SECRET, refresh);
    int status;
    char *response = http_request(GCAL_TOKEN_URL, body, NULL, &status);
    bool ok = response && status == 200 && tokens_take(response);
    if (!ok) {
        ESP_LOGW(TAG, "Token not refreshed (HTTP %d): %.200s", status, response ? response : "");
        // "invalid_grant": the access was withdrawn (in the Google account) or expired: to link again
        *revoked = response && strstr(response, "invalid_grant") != NULL;
    }
    free(response);
    return ok ? s_access : NULL;
}

/* ---------- linking ---------- */

void gcal_link_start(void)
{
    s_state = GCAL_LINK_ASKING;
    s_start = true;
}

void gcal_link_cancel(void)
{
    s_start = false;
    s_state = GCAL_LINK_IDLE;
}

gcal_link_state_t gcal_link_state(const char **url, const char **code)
{
    *url = s_url;
    *code = s_user_code;
    return s_state;
}

/* The code asked to Google: shown with its address */
static void ask_code(void)
{
    char body[300];
    snprintf(body, sizeof(body), "client_id=%s&scope=%s", GCAL_CLIENT_ID, "https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fcalendar.readonly");
    int status;
    char *response = http_request(GCAL_DEVICE_URL, body, NULL, &status);
    cJSON *json = response && status == 200 ? cJSON_Parse(response) : NULL;
    const char *device = json_string(json, "device_code");
    const char *user = json_string(json, "user_code");
    const char *url = json_string(json, "verification_url");
    const cJSON *expires = cJSON_GetObjectItem(json, "expires_in");
    const cJSON *interval = cJSON_GetObjectItem(json, "interval");
    if (s_state == GCAL_LINK_ASKING && device && user) {
        strlcpy(s_device_code, device, sizeof(s_device_code));
        strlcpy(s_user_code, user, sizeof(s_user_code));
        strlcpy(s_url, url ? url : "https://www.google.com/device", sizeof(s_url));
        s_interval_s = cJSON_IsNumber(interval) && interval->valueint > 0 ? interval->valueint : 5;
        s_expires_us = esp_timer_get_time() + (cJSON_IsNumber(expires) ? expires->valueint : 1800) * 1000000LL;
        s_next_poll_us = esp_timer_get_time() + s_interval_s * 1000000LL;
        s_state = GCAL_LINK_WAITING;
        ESP_LOGI(TAG, "Code %s to type at %s (%d s)", s_user_code, s_url, cJSON_IsNumber(expires) ? expires->valueint : 0);
    } else if (s_state == GCAL_LINK_ASKING) {
        ESP_LOGE(TAG, "No code (HTTP %d): %.300s", status, response ? response : "");
        s_state = GCAL_LINK_FAILED;
    }
    cJSON_Delete(json);
    free(response);
}

/* Whether the user accepted, at the pace Google asks */
static void poll_token(void)
{
    if (esp_timer_get_time() > s_expires_us) {
        s_state = GCAL_LINK_EXPIRED;
        return;
    }
    if (esp_timer_get_time() < s_next_poll_us) {
        return;
    }
    s_next_poll_us = esp_timer_get_time() + s_interval_s * 1000000LL;
    char body[500];
    snprintf(body, sizeof(body), "client_id=%s&client_secret=%s&device_code=%s"
             "&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code",
             GCAL_CLIENT_ID, GCAL_CLIENT_SECRET, s_device_code);
    int status;
    char *response = http_request(GCAL_TOKEN_URL, body, NULL, &status);
    if (response && status == 200) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool ok = tokens_take(response);
        xSemaphoreGive(s_lock);
        if (ok && s_refresh && s_state == GCAL_LINK_WAITING) {
            s_state = GCAL_LINK_DONE;
            ESP_LOGI(TAG, "Linked to a Google account");
        }
    } else if (response && strstr(response, "slow_down")) {
        s_interval_s += 5;
    } else if (response && strstr(response, "access_denied")) {
        s_state = GCAL_LINK_DENIED;
        ESP_LOGI(TAG, "The user refused");
    } else if (response && strstr(response, "expired_token")) {
        s_state = GCAL_LINK_EXPIRED;
    } else if (!response || !strstr(response, "authorization_pending")) {
        ESP_LOGW(TAG, "Token not given (HTTP %d): %.200s", status, response ? response : "");
    }
    free(response);
}

void gcal_poll(void)
{
    if (!gcal_configured()) {
        return;
    }
    if (s_unlink) {
        unlink_now();
    } else if (s_start) {
        s_start = false;
        ask_code();
    } else if (s_state == GCAL_LINK_WAITING) {
        poll_token();
    }
}

void gcal_unlink(void)
{
    s_unlink = true;        // done by the main task (it calls Google); not linked from now on
}

static void unlink_now(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_refresh) {
        // Revoked at Google too (the device no longer appears among the account's connected apps)
        char token[600], body[700];
        url_encode(token, sizeof(token), s_refresh);
        snprintf(body, sizeof(body), "token=%s", token);
        int status;
        free(http_request(GCAL_REVOKE_URL, body, NULL, &status));
        ESP_LOGI(TAG, "Unlinked (revoked: HTTP %d)", status);
    }
    free(s_refresh);
    free(s_access);
    s_refresh = NULL;
    s_access = NULL;
    remove(GCAL_TOKEN_FILE);
    xSemaphoreGive(s_lock);
    s_state = GCAL_LINK_IDLE;
    s_unlink = false;
}

/* ---------- the model's tool ---------- */

const char *gcal_tool_definition(void)
{
    if (!gcal_configured()) {
        return NULL;
    }
    return "{\"name\":\"calendar_events\","
           "\"description\":\"The events of the user's Google Calendar (main calendar) for one day or a few days: "
           "start and end times (local time, or all_day), title, place. Use it for any question about the user's "
           "agenda, appointments, meetings or schedule ('what do I have today', 'am I free tomorrow afternoon'). "
           "Today's date is in the brackets at the start of the user's message. Say the events briefly, in time "
           "order; do not read out ids or links.\","
           "\"input_schema\":{\"type\":\"object\",\"properties\":{"
           "\"date\":{\"type\":\"string\",\"description\":\"First day, YYYY-MM-DD; today when left out\"},"
           "\"days\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":7,\"description\":\"Number of days from that "
           "date, 1 by default\"}}}}";
}

/* "2026-11-05T14:00:00+01:00" -> "2026-11-05", "14:00" */
static void split_time(const char *value, char *day, size_t day_size, char *hour, size_t hour_size)
{
    snprintf(day, day_size, "%.10s", value);
    hour[0] = '\0';
    if (strlen(value) >= 16 && value[10] == 'T') {
        snprintf(hour, hour_size, "%.5s", value + 11);
    }
}

char *gcal_events(const char *date, int days, bool *is_error)
{
    *is_error = true;
    if (!gcal_configured()) {
        return strdup("{\"error\":\"Google Calendar is not available in this firmware\"}");
    }
    if (!gcal_linked()) {
        return strdup("{\"error\":\"no Google Calendar linked: the user can link one on the Settings screen "
                      "(Parametres page, Google Calendar, Connect)\"}");
    }
    // The days asked, from local midnight, in UTC for Google ("2026-11-04T23:00:00Z")
    days = days < 1 ? 1 : (days > GCAL_DAYS_MAX ? GCAL_DAYS_MAX : days);
    time_t now = time(NULL);
    struct tm day;
    localtime_r(&now, &day);
    if (date && strlen(date) >= 10) {
        sscanf(date, "%4d-%2d-%2d", &day.tm_year, &day.tm_mon, &day.tm_mday);
        day.tm_year -= 1900;
        day.tm_mon -= 1;
    }
    day.tm_hour = day.tm_min = day.tm_sec = 0;
    day.tm_isdst = -1;
    time_t from = mktime(&day);
    day.tm_mday += days;
    day.tm_isdst = -1;
    time_t to = mktime(&day);
    struct tm utc;
    char from_text[24], to_text[24];
    strftime(from_text, sizeof(from_text), "%Y-%m-%dT%H:%M:%SZ", gmtime_r(&from, &utc));
    strftime(to_text, sizeof(to_text), "%Y-%m-%dT%H:%M:%SZ", gmtime_r(&to, &utc));
    char url[400];
    snprintf(url, sizeof(url), GCAL_EVENTS_URL "?timeMin=%s&timeMax=%s&singleEvents=true&orderBy=startTime"
             "&maxResults=%d&fields=items(summary,location,start,end)", from_text, to_text, GCAL_EVENTS_MAX);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool revoked;
    const char *token = access_token(&revoked);
    char *bearer = token ? strdup(token) : NULL;
    xSemaphoreGive(s_lock);
    if (!bearer) {
        return strdup(revoked ? "{\"error\":\"the access to Google Calendar was withdrawn or expired: the user must "
                      "link it again on the Settings screen\"}" : "{\"error\":\"Google cannot be reached\"}");
    }
    int64_t t0 = esp_timer_get_time();
    int status;
    char *response = http_request(url, NULL, bearer, &status);
    free(bearer);
    cJSON *json = response && status == 200 ? cJSON_Parse(response) : NULL;
    ESP_LOGI(TAG, "Events %.10s, %d day(s): HTTP %d in %lld ms", from_text, days, status,
             (esp_timer_get_time() - t0) / 1000);
    if (!json) {
        ESP_LOGW(TAG, "%.300s", response ? response : "");
        free(response);
        return strdup("{\"error\":\"Google Calendar did not answer\"}");
    }
    free(response);

    // Compact for the model: date, start, end (or all_day), title, place
    cJSON *out = cJSON_CreateObject();
    char first_day[12];
    strftime(first_day, sizeof(first_day), "%Y-%m-%d", localtime_r(&from, &utc));
    cJSON_AddStringToObject(out, "from", first_day);
    cJSON_AddNumberToObject(out, "days", days);
    cJSON *list = cJSON_AddArrayToObject(out, "events");
    const cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(json, "items")) {
        const cJSON *start = cJSON_GetObjectItem(item, "start");
        const cJSON *end = cJSON_GetObjectItem(item, "end");
        const char *start_at = json_string(start, "dateTime");
        cJSON *event = cJSON_CreateObject();
        char d[12], h[8];
        if (start_at) {
            split_time(start_at, d, sizeof(d), h, sizeof(h));
            cJSON_AddStringToObject(event, "date", d);
            cJSON_AddStringToObject(event, "start", h);
            const char *end_at = json_string(end, "dateTime");
            if (end_at) {
                split_time(end_at, d, sizeof(d), h, sizeof(h));
                cJSON_AddStringToObject(event, "end", h);
            }
        } else {
            const char *start_day = json_string(start, "date");
            cJSON_AddStringToObject(event, "date", start_day ? start_day : "");
            cJSON_AddBoolToObject(event, "all_day", true);
        }
        const char *title = json_string(item, "summary");
        const char *place = json_string(item, "location");
        cJSON_AddStringToObject(event, "title", title ? title : "(no title)");
        if (place && place[0]) {
            cJSON_AddStringToObject(event, "place", place);
        }
        cJSON_AddItemToArray(list, event);
    }
    cJSON_Delete(json);
    char *text = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    *is_error = false;
    return text ? text : strdup("{\"events\":[]}");
}
