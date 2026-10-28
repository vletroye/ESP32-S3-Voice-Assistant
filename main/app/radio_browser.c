/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Radio-Browser client. The API is reached over plain http (no TLS: no internal RAM taken from the conversation),
 * on one of its servers: the list given by all.api.radio-browser.info (read once), taken in a random order, the
 * next one being tried when one does not answer, as the API asks; a few known servers when the list cannot be read.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "radio_browser.h"

#define RB_USER_AGENT       "ESP32-S3-Voice-Assistant/1.0"
#define RB_SERVERS_URL      "http://all.api.radio-browser.info/json/servers"
#define RB_SERVERS_MAX      8
#define RB_HOST_SIZE        64
#define RB_TIMEOUT_MS       6000
#define RB_RESPONSE_MAX     (192 * 1024)
#define RB_FETCHED          40          /* stations asked for: some are left out (codec, https, duplicates) */
#define RB_BITRATE_MAX      192

static const char *TAG = "radio_browser";

/* Used when the list of servers cannot be read */
static const char *const s_fallback_servers[] = {
    "de1.api.radio-browser.info", "de2.api.radio-browser.info", "fi1.api.radio-browser.info",
};

static char (*s_servers)[RB_HOST_SIZE] = NULL;     /* PSRAM */
static int s_server_count = 0;
static int s_server_next = 0;                       /* the one that answered last is tried first */

/* The body of an http GET (PSRAM, NUL-terminated, to be freed), NULL on failure */
static char *http_get(const char *url, int *status)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = RB_TIMEOUT_MS,
        .buffer_size = 2048,
        .user_agent = RB_USER_AGENT,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    char *body = heap_caps_malloc(RB_RESPONSE_MAX, MALLOC_CAP_SPIRAM);
    *status = -1;
    bool ok = false;
    if (client && body && esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
        *status = esp_http_client_get_status_code(client);
        size_t len = 0;
        int n;
        while (len < RB_RESPONSE_MAX - 1 &&
               (n = esp_http_client_read(client, body + len, RB_RESPONSE_MAX - 1 - len)) > 0) {
            len += n;
        }
        body[len] = '\0';
        ok = (*status == 200 && len > 0);
    }
    if (client) {
        esp_http_client_cleanup(client);
    }
    if (!ok) {
        free(body);
        return NULL;
    }
    return body;
}

/* The servers of the API, in a random order (read once) */
static void servers_init(void)
{
    if (s_servers) {
        return;
    }
    s_servers = heap_caps_calloc(RB_SERVERS_MAX, RB_HOST_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_servers) {
        return;
    }
    int status = 0;
    char *body = http_get(RB_SERVERS_URL, &status);
    cJSON *list = body ? cJSON_Parse(body) : NULL;
    free(body);
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        const cJSON *name = cJSON_GetObjectItem(item, "name");
        if (!cJSON_IsString(name) || s_server_count >= RB_SERVERS_MAX) {
            continue;
        }
        bool known = false;     // each server is listed once per address (IPv4, IPv6)
        for (int i = 0; i < s_server_count; i++) {
            known |= strcasecmp(s_servers[i], name->valuestring) == 0;
        }
        if (!known) {
            strlcpy(s_servers[s_server_count++], name->valuestring, RB_HOST_SIZE);
        }
    }
    cJSON_Delete(list);
    if (s_server_count == 0) {
        ESP_LOGW(TAG, "Servers not listed (HTTP %d): known ones used", status);
        for (size_t i = 0; i < sizeof(s_fallback_servers) / sizeof(s_fallback_servers[0]); i++) {
            strlcpy(s_servers[s_server_count++], s_fallback_servers[i], RB_HOST_SIZE);
        }
    }
    // Random order, as the API asks (the load is spread over its servers)
    for (int i = s_server_count - 1; i > 0; i--) {
        int j = esp_random() % (i + 1);
        char tmp[RB_HOST_SIZE];
        strlcpy(tmp, s_servers[i], sizeof(tmp));
        strlcpy(s_servers[i], s_servers[j], RB_HOST_SIZE);
        strlcpy(s_servers[j], tmp, RB_HOST_SIZE);
    }
    ESP_LOGI(TAG, "%d server(s), first %s", s_server_count, s_servers[0]);
}

/* GET of a path of the API ("/json/..."), trying the servers in turn; NULL when none answered */
static char *api_get(const char *path)
{
    servers_init();
    for (int tried = 0; tried < s_server_count; tried++) {
        int i = (s_server_next + tried) % s_server_count;
        char url[512];
        snprintf(url, sizeof(url), "http://%s%s", s_servers[i], path);
        int status = 0;
        char *body = http_get(url, &status);
        if (body) {
            s_server_next = i;
            return body;
        }
        ESP_LOGW(TAG, "%s did not answer (HTTP %d)", s_servers[i], status);
    }
    return NULL;
}

/* "rock & roll" -> "rock%20%26%20roll" */
static void url_encode(const char *in, char *out, size_t size)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && n + 4 < size; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[n++] = *p;
        } else {
            n += snprintf(out + n, size - n, "%%%02X", *p);
        }
    }
    out[n] = '\0';
}

static void add_param(char *path, size_t size, const char *key, const char *value)
{
    if (value && value[0]) {
        char encoded[160];
        url_encode(value, encoded, sizeof(encoded));
        size_t n = strlen(path);
        snprintf(path + n, size - n, "&%s=%s", key, encoded);
    }
}

static const char *json_string(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}

static int json_int(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : 0;
}

/* A stream the radio can play: MP3 or AAC, http, not HLS, online at the last check, not too heavy */
static bool playable(const cJSON *st, const char **why)
{
    const char *codec = json_string(st, "codec");
    const char *url = json_string(st, "url_resolved");
    if (json_int(st, "hls")) {
        *why = "HLS";
    } else if (strcasecmp(codec, "MP3") != 0 && strncasecmp(codec, "AAC", 3) != 0) {
        *why = "codec";
    } else if (strncmp(url, "http://", 7) != 0) {
        *why = "not http";
    } else if (strlen(url) >= RADIO_URL_SIZE) {
        *why = "URL too long";
    } else if (json_int(st, "bitrate") > RB_BITRATE_MAX) {
        *why = "bitrate";
    } else if (!json_int(st, "lastcheckok")) {
        *why = "offline";
    } else {
        return true;
    }
    return false;
}

/* The name as shown and said: spaces at the ends removed */
static void clean_name(const char *in, char *out, size_t size)
{
    while (*in == ' ' || *in == '\t') {
        in++;
    }
    strlcpy(out, in, size);
    size_t n = strlen(out);
    while (n && (out[n - 1] == ' ' || out[n - 1] == '\t' || out[n - 1] == '\n')) {
        out[--n] = '\0';
    }
}

int radio_browser_search(const char *name, const char *tag, const char *countrycode, radio_station_t *out, int max)
{
    int64_t t0 = esp_timer_get_time();
    char path[512];
    snprintf(path, sizeof(path), "/json/stations/search?hidebroken=true&order=clickcount&reverse=true&limit=%d",
             RB_FETCHED);
    add_param(path, sizeof(path), "name", name);
    add_param(path, sizeof(path), "tag", tag);
    add_param(path, sizeof(path), "countrycode", countrycode);
    char *body = api_get(path);
    if (!body) {
        ESP_LOGE(TAG, "No server answered");
        return -1;
    }
    cJSON *list = cJSON_Parse(body);
    free(body);
    int found = 0, read = 0, left_out = 0;
    const cJSON *st;
    cJSON_ArrayForEach(st, list) {
        read++;
        const char *why = NULL;
        if (found >= max) {
            break;
        }
        if (!playable(st, &why)) {
            left_out++;
            ESP_LOGD(TAG, "Left out (%s): %s", why, json_string(st, "name"));
            continue;
        }
        char station_name[RADIO_NAME_SIZE];
        clean_name(json_string(st, "name"), station_name, sizeof(station_name));
        bool duplicate = !station_name[0];
        for (int i = 0; i < found && !duplicate; i++) {
            duplicate = strcasecmp(out[i].name, station_name) == 0;
        }
        if (duplicate) {
            left_out++;
            continue;
        }
        radio_station_t *s = &out[found++];
        strlcpy(s->name, station_name, sizeof(s->name));
        strlcpy(s->url, json_string(st, "url_resolved"), sizeof(s->url));
        strlcpy(s->uuid, json_string(st, "stationuuid"), sizeof(s->uuid));
    }
    cJSON_Delete(list);
    ESP_LOGI(TAG, "Search name=\"%s\" tag=\"%s\" country=\"%s\": %d station(s) read, %d kept, %d left out, in %lld ms",
             name ? name : "", tag ? tag : "", countrycode ? countrycode : "", read, found, left_out,
             (esp_timer_get_time() - t0) / 1000);
    for (int i = 0; i < found; i++) {
        ESP_LOGI(TAG, "  %2d. %s", i + 1, out[i].name);
    }
    return found;
}

void radio_browser_click(const char *uuid)
{
    if (!uuid || !uuid[0]) {
        return;
    }
    char path[96];
    snprintf(path, sizeof(path), "/json/url/%s", uuid);
    free(api_get(path));
}
