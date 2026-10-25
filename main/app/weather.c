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

#define WEATHER_TIMEOUT_MS      8000
#define WEATHER_MAX_BODY        (24 * 1024)
#define GEOCODING_URL           "https://geocoding-api.open-meteo.com/v1/search"
#define FORECAST_URL            "https://api.open-meteo.com/v1/forecast"

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

char *weather_get(const char *location, int days, const char *lang, bool *is_error)
{
    *is_error = false;
    if (!location || !location[0]) {
        return error_result("No place given: ask the user where", is_error);
    }
    days = days < 1 ? 1 : (days > 7 ? 7 : days);
    int64_t t0 = esp_timer_get_time();

    // "Namur, Belgique": the name is searched, the country picks among the places found
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
    const cJSON *results = cJSON_GetObjectItem(geo, "results");
    const cJSON *place = cJSON_GetArrayItem(results, 0);
    if (country && country[0]) {
        const cJSON *r;
        cJSON_ArrayForEach(r, results) {
            const cJSON *c = cJSON_GetObjectItem(r, "country");
            if (cJSON_IsString(c) && strncasecmp(c->valuestring, country, strlen(country)) == 0) {
                place = r;
                break;
            }
        }
    }
    const cJSON *lat = cJSON_GetObjectItem(place, "latitude");
    const cJSON *lon = cJSON_GetObjectItem(place, "longitude");
    if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) {
        cJSON_Delete(geo);
        return error_result(geo ? "Place not found" : "Weather service unreachable", is_error);
    }

    cJSON *result = cJSON_CreateObject();
    char place_name[160];
    const cJSON *pname = cJSON_GetObjectItem(place, "name");
    const cJSON *admin = cJSON_GetObjectItem(place, "admin1");
    const cJSON *pcountry = cJSON_GetObjectItem(place, "country");
    snprintf(place_name, sizeof(place_name), "%s%s%s%s%s", cJSON_IsString(pname) ? pname->valuestring : name,
             cJSON_IsString(admin) ? ", " : "", cJSON_IsString(admin) ? admin->valuestring : "",
             cJSON_IsString(pcountry) ? ", " : "", cJSON_IsString(pcountry) ? pcountry->valuestring : "");
    cJSON_AddStringToObject(result, "place", place_name);

    snprintf(url, sizeof(url), FORECAST_URL "?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m,precipitation"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,"
             "precipitation_sum&timezone=auto&forecast_days=%d", lat->valuedouble, lon->valuedouble, days);
    cJSON_Delete(geo);
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
    ESP_LOGI(TAG, "%s, %d day(s) in %lld ms", place_name, days, (esp_timer_get_time() - t0) / 1000);
    return text;
}
