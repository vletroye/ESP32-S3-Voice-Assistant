/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "llm_sse.h"
#include "weather.h"
#include "app_memory.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define WEATHER_TIMEOUT_MS      8000
#define WEATHER_MAX_BODY        (24 * 1024)
#define GEOCODING_URL           "https://geocoding-api.open-meteo.com/v1/search"
#define FORECAST_URL            "https://api.open-meteo.com/v1/forecast"
/* Places already looked up (a city does not move): saved in this file, read once at start-up */
#define PLACES_FILE             "/spiffs/places.json"
#define PLACES_FILE_MAX         2048
#define PLACES_MAX              8

static const char *TAG = "weather";

/* WMO weather interpretation codes used by Open-Meteo */
static const char *wmo_text(int code)
{
    switch (code) {
    case 0: return "clear sky";
    case 1: return "mainly clear";
    case 2: return "partly cloudy";
    case 3: return "overcast";
    case 45: case 48: return "fog";
    case 51: case 53: case 55: return "drizzle";
    case 56: case 57: return "freezing drizzle";
    case 61: return "light rain";
    case 63: return "rain";
    case 65: return "heavy rain";
    case 66: case 67: return "freezing rain";
    case 71: return "light snow";
    case 73: return "snow";
    case 75: return "heavy snow";
    case 77: return "snow grains";
    case 80: case 81: return "rain showers";
    case 82: return "violent rain showers";
    case 85: case 86: return "snow showers";
    case 95: return "thunderstorm";
    case 96: case 99: return "thunderstorm with hail";
    default: return "unknown";
    }
}

/* GET a URL, the body as a parsed JSON tree (NULL on error) */
static cJSON *http_get_json(const char *url)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = WEATHER_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return NULL;
    }
    cJSON *json = NULL;
    llm_buf_t body = {0};
    if (esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
        int status = esp_http_client_get_status_code(client);
        char chunk[512];
        int n;
        while ((n = esp_http_client_read(client, chunk, sizeof(chunk))) > 0 && body.len < WEATHER_MAX_BODY) {
            llm_buf_append(&body, chunk, n);
        }
        if (status == 200 && body.len) {
            json = cJSON_Parse(body.p);
        } else {
            ESP_LOGE(TAG, "HTTP %d: %.200s", status, body.len ? body.p : "");
        }
    } else {
        ESP_LOGE(TAG, "Request failed");
    }
    llm_buf_free(&body);
    esp_http_client_cleanup(client);
    return json;
}

/* Percent-encode a query parameter (UTF-8 bytes included) */
static void url_encode(const char *in, char *out, size_t size)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && n + 4 < size; p++) {
        if (isalnum(*p) || strchr("-_.~", *p)) {
            out[n++] = *p;
        } else {
            n += snprintf(out + n, size - n, "%%%02X", *p);
        }
    }
    out[n] = '\0';
}

static char *error_result(const char *message, bool *is_error)
{
    *is_error = true;
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", message);
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    return text;
}

/* ---------- places: geocoding results kept in PSRAM, saved to flash when idle ---------- */

typedef struct {
    char key[72];           /* language + normalized place name as asked, e.g. "fr|namur belgique" */
    char name[96];          /* full name given to the model, e.g. "Namur, Wallonie, Belgique" */
    double lat;
    double lon;
    uint32_t used;          /* last use (higher is more recent), to replace the oldest place */
} place_t;

static place_t s_places[PLACES_MAX];
static int s_place_count = 0;
static uint32_t s_place_clock = 0;
static bool s_places_dirty = false;
static SemaphoreHandle_t s_places_lock = NULL;

static void place_key(const char *location, const char *lang, char *key, size_t size)
{
    char norm[64];
    app_text_normalize(location, norm, sizeof(norm));
    snprintf(key, size, "%s|%s", lang, norm);
}

static bool place_find(const char *location, const char *lang, place_t *out)
{
    char key[72];
    place_key(location, lang, key, sizeof(key));
    bool found = false;
    xSemaphoreTake(s_places_lock, portMAX_DELAY);
    for (int i = 0; i < s_place_count; i++) {
        if (strcmp(s_places[i].key, key) == 0) {
            s_places[i].used = ++s_place_clock;
            *out = s_places[i];
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_places_lock);
    ESP_LOGI(TAG, "place \"%s\": %s", key, found ? "known" : "to look up");
    return found;
}

static void place_add(place_t *place)
{
    xSemaphoreTake(s_places_lock, portMAX_DELAY);
    int slot = s_place_count;
    if (s_place_count < PLACES_MAX) {
        s_place_count++;
    } else {
        slot = 0;   // full: the least recently used place is replaced
        for (int i = 1; i < PLACES_MAX; i++) {
            if (s_places[i].used < s_places[slot].used) {
                slot = i;
            }
        }
    }
    place->used = ++s_place_clock;
    s_places[slot] = *place;
    s_places_dirty = true;
    xSemaphoreGive(s_places_lock);
}

void weather_places_load(void)
{
    if (!s_places_lock) {
        s_places_lock = xSemaphoreCreateMutex();
    }
    FILE *f = fopen(PLACES_FILE, "r");
    if (!f) {
        return;
    }
    char *text = calloc(1, PLACES_FILE_MAX);
    size_t n = text ? fread(text, 1, PLACES_FILE_MAX - 1, f) : 0;
    fclose(f);
    cJSON *json = n ? cJSON_Parse(text) : NULL;
    free(text);
    const cJSON *item;
    cJSON_ArrayForEach(item, json) {
        const cJSON *k = cJSON_GetObjectItem(item, "k");
        const cJSON *name = cJSON_GetObjectItem(item, "n");
        const cJSON *lat = cJSON_GetObjectItem(item, "lat");
        const cJSON *lon = cJSON_GetObjectItem(item, "lon");
        if (s_place_count < PLACES_MAX && cJSON_IsString(k) && cJSON_IsString(name) && cJSON_IsNumber(lat) &&
                cJSON_IsNumber(lon)) {
            place_t *p = &s_places[s_place_count++];
            strlcpy(p->key, k->valuestring, sizeof(p->key));
            strlcpy(p->name, name->valuestring, sizeof(p->name));
            p->lat = lat->valuedouble;
            p->lon = lon->valuedouble;
            p->used = ++s_place_clock;
        }
    }
    cJSON_Delete(json);
    ESP_LOGI(TAG, "%d known place(s)", s_place_count);
}

void weather_places_save(void)
{
    if (!s_places_lock || !s_places_dirty) {
        return;
    }
    xSemaphoreTake(s_places_lock, portMAX_DELAY);
    cJSON *json = cJSON_CreateArray();
    for (int i = 0; i < s_place_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "k", s_places[i].key);
        cJSON_AddStringToObject(item, "n", s_places[i].name);
        cJSON_AddNumberToObject(item, "lat", s_places[i].lat);
        cJSON_AddNumberToObject(item, "lon", s_places[i].lon);
        cJSON_AddItemToArray(json, item);
    }
    s_places_dirty = false;
    xSemaphoreGive(s_places_lock);
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    FILE *f = text ? fopen(PLACES_FILE, "w") : NULL;
    if (f) {
        fputs(text, f);
        fclose(f);
        ESP_LOGI(TAG, "places saved (%u bytes)", (unsigned)strlen(text));
    }
    free(text);
}

/* Name to coordinates with the geocoding service; "Namur, Belgique": the country picks among the results */
static bool geocode(const char *location, const char *lang, place_t *place, bool *reachable)
{
    char name[64];
    strlcpy(name, location, sizeof(name));
    char *comma = strchr(name, ',');
    const char *country = NULL;
    if (comma) {
        *comma = '\0';
        country = location + (comma - name) + 1;
        while (*country == ' ') {
            country++;
        }
    }
    char encoded[192];
    url_encode(name, encoded, sizeof(encoded));
    char url[320];
    snprintf(url, sizeof(url), GEOCODING_URL "?name=%s&count=5&language=%s&format=json", encoded, lang);
    cJSON *geo = http_get_json(url);
    *reachable = (geo != NULL);
    const cJSON *results = cJSON_GetObjectItem(geo, "results");
    const cJSON *found = cJSON_GetArrayItem(results, 0);
    if (country && country[0]) {
        const cJSON *r;
        cJSON_ArrayForEach(r, results) {
            const cJSON *c = cJSON_GetObjectItem(r, "country");
            if (cJSON_IsString(c) && strncasecmp(c->valuestring, country, strlen(country)) == 0) {
                found = r;
                break;
            }
        }
    }
    const cJSON *lat = cJSON_GetObjectItem(found, "latitude");
    const cJSON *lon = cJSON_GetObjectItem(found, "longitude");
    bool ok = cJSON_IsNumber(lat) && cJSON_IsNumber(lon);
    if (ok) {
        memset(place, 0, sizeof(*place));
        place_key(location, lang, place->key, sizeof(place->key));
        const cJSON *pname = cJSON_GetObjectItem(found, "name");
        const cJSON *admin = cJSON_GetObjectItem(found, "admin1");
        const cJSON *pcountry = cJSON_GetObjectItem(found, "country");
        snprintf(place->name, sizeof(place->name), "%s%s%s%s%s", cJSON_IsString(pname) ? pname->valuestring : name,
                 cJSON_IsString(admin) ? ", " : "", cJSON_IsString(admin) ? admin->valuestring : "",
                 cJSON_IsString(pcountry) ? ", " : "", cJSON_IsString(pcountry) ? pcountry->valuestring : "");
        place->lat = lat->valuedouble;
        place->lon = lon->valuedouble;
    }
    cJSON_Delete(geo);
    return ok;
}

char *weather_get(const char *location, int days, const char *lang, bool *is_error)
{
    *is_error = false;
    if (!location || !location[0]) {
        return error_result("No place given: ask the user where", is_error);
    }
    days = days < 1 ? 1 : (days > 7 ? 7 : days);
    int64_t t0 = esp_timer_get_time();

    place_t place;
    if (!place_find(location, lang, &place)) {
        bool reachable = true;
        if (!geocode(location, lang, &place, &reachable)) {
            return error_result(reachable ? "Place not found" : "Weather service unreachable", is_error);
        }
        place_add(&place);
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "place", place.name);

    char url[320];
    snprintf(url, sizeof(url), FORECAST_URL "?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m,precipitation"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,"
             "precipitation_sum&timezone=auto&forecast_days=%d", place.lat, place.lon, days);
    cJSON *forecast = http_get_json(url);
    if (!forecast) {
        cJSON_Delete(result);
        return error_result("Weather service unreachable", is_error);
    }

    const cJSON *cur = cJSON_GetObjectItem(forecast, "current");
    if (cur) {
        cJSON *now = cJSON_AddObjectToObject(result, "now");
        const cJSON *v;
        if ((v = cJSON_GetObjectItem(cur, "temperature_2m")) && cJSON_IsNumber(v)) {
            cJSON_AddNumberToObject(now, "temperature_c", v->valuedouble);
        }
        if ((v = cJSON_GetObjectItem(cur, "apparent_temperature")) && cJSON_IsNumber(v)) {
            cJSON_AddNumberToObject(now, "feels_like_c", v->valuedouble);
        }
        if ((v = cJSON_GetObjectItem(cur, "weather_code")) && cJSON_IsNumber(v)) {
            cJSON_AddStringToObject(now, "conditions", wmo_text(v->valueint));
        }
        if ((v = cJSON_GetObjectItem(cur, "wind_speed_10m")) && cJSON_IsNumber(v)) {
            cJSON_AddNumberToObject(now, "wind_kmh", v->valuedouble);
        }
        if ((v = cJSON_GetObjectItem(cur, "precipitation")) && cJSON_IsNumber(v)) {
            cJSON_AddNumberToObject(now, "precipitation_mm", v->valuedouble);
        }
    }

    const cJSON *daily = cJSON_GetObjectItem(forecast, "daily");
    const cJSON *dates = cJSON_GetObjectItem(daily, "time");
    cJSON *out_days = cJSON_AddArrayToObject(result, "days");
    for (int i = 0; i < cJSON_GetArraySize(dates); i++) {
        cJSON *day = cJSON_CreateObject();
        const cJSON *v = cJSON_GetArrayItem(dates, i);
        if (cJSON_IsString(v)) {
            cJSON_AddStringToObject(day, "date", v->valuestring);
        }
        if ((v = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "weather_code"), i)) && cJSON_IsNumber(v)) {
            cJSON_AddStringToObject(day, "conditions", wmo_text(v->valueint));
        }
        if ((v = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "temperature_2m_min"), i)) && cJSON_IsNumber(v)) {
            cJSON_AddNumberToObject(day, "min_c", v->valuedouble);
        }
        if ((v = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "temperature_2m_max"), i)) && cJSON_IsNumber(v)) {
            cJSON_AddNumberToObject(day, "max_c", v->valuedouble);
        }
        if ((v = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "precipitation_probability_max"), i)) &&
                cJSON_IsNumber(v)) {
            cJSON_AddNumberToObject(day, "rain_probability_percent", v->valuedouble);
        }
        if ((v = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "precipitation_sum"), i)) && cJSON_IsNumber(v)) {
            cJSON_AddNumberToObject(day, "precipitation_mm", v->valuedouble);
        }
        cJSON_AddItemToArray(out_days, day);
    }
    cJSON_Delete(forecast);

    char *text = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    ESP_LOGI(TAG, "%s, %d day(s) in %lld ms", place.name, days, (esp_timer_get_time() - t0) / 1000);
    return text;
}
