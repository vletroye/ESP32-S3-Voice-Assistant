/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "file_memory.h"
#include "file_radio_favorites.h"

#define FAV_FILE_MAX    16384       /* as on the configuration drive */
#define FAV_NORM_SIZE   96

static const char *TAG = "radio_fav";

static const char s_header[] =
    "# Radios favorites / favourite radios (RADIO.TXT): une par ligne / one per line\n"
    "#   nom | URL du flux | id Radio-Browser (facultatif)\n"
    "#   name | stream URL | Radio-Browser id (optional)\n"
    "# Ajoutees et retirees a la voix (\"ajoute cette radio aux favoris\") ou depuis l'ecran Radio; une radio\n"
    "# absente de Radio-Browser peut etre ajoutee ici a la main (flux MP3 ou AAC en http).\n"
    "# Added and removed by voice (\"add this radio to my favourites\") or from the radio screen; a radio missing\n"
    "# from Radio-Browser can be added here by hand (MP3 or AAC stream over http).\n";

static radio_station_t *s_favorites = NULL;    /* PSRAM, RADIO_LIST_MAX */
static int s_count = 0;
static SemaphoreHandle_t s_lock = NULL;

static char *read_file(void)
{
    FILE *f = fopen(RADIO_FAVORITES_FILE, "r");
    if (!f) {
        return NULL;
    }
    char *text = heap_caps_malloc(FAV_FILE_MAX, MALLOC_CAP_SPIRAM);
    size_t n = text ? fread(text, 1, FAV_FILE_MAX - 1, f) : 0;
    fclose(f);
    if (text) {
        text[n] = '\0';
    }
    return text;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
    return s;
}

/* "name | url | uuid" into station; false for a comment, a blank line or a line without an http URL */
static bool parse_line(char *line, radio_station_t *station)
{
    char *s = trim(line);
    if (!s[0] || s[0] == '#') {
        return false;
    }
    char *fields[3] = { s, NULL, NULL };
    for (int i = 1; i < 3; i++) {
        char *bar = fields[i - 1] ? strchr(fields[i - 1], '|') : NULL;
        if (bar) {
            *bar = '\0';
            fields[i] = bar + 1;
        }
    }
    const char *url = fields[1] ? trim(fields[1]) : "";
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        ESP_LOGW(TAG, "Line left out (\"name | URL\" expected): %s", s);
        return false;
    }
    memset(station, 0, sizeof(*station));
    strlcpy(station->name, trim(fields[0]), sizeof(station->name));
    strlcpy(station->url, url, sizeof(station->url));
    if (fields[2]) {
        strlcpy(station->uuid, trim(fields[2]), sizeof(station->uuid));
    }
    return true;
}

static void load(void)
{
    s_count = 0;
    char *text = read_file();
    for (char *line = text; line && *line && s_count < RADIO_LIST_MAX;) {
        char *end = strchr(line, '\n');
        if (end) {
            *end = '\0';
        }
        if (parse_line(line, &s_favorites[s_count])) {
            s_count++;
        }
        line = end ? end + 1 : NULL;
    }
    free(text);
}

void radio_favorites_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        s_favorites = heap_caps_calloc(RADIO_LIST_MAX, sizeof(radio_station_t), MALLOC_CAP_SPIRAM);
    }
    FILE *f = fopen(RADIO_FAVORITES_FILE, "r");
    if (f) {
        fclose(f);
    } else if ((f = fopen(RADIO_FAVORITES_FILE, "w")) != NULL) {
        fputs(s_header, f);
        fclose(f);
        ESP_LOGI(TAG, "%s made", RADIO_FAVORITES_FILE);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    load();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%d favourite radio(s)", s_count);
}

int radio_favorites_list(radio_station_t *out, int max)
{
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_count < max ? s_count : max;
    memcpy(out, s_favorites, n * sizeof(radio_station_t));
    xSemaphoreGive(s_lock);
    return n;
}

int radio_favorites_count(void)
{
    return s_count;
}

static int index_of(const radio_station_t *station)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_favorites[i].url, station->url) == 0 ||
            (station->uuid[0] && strcmp(s_favorites[i].uuid, station->uuid) == 0)) {
            return i;
        }
    }
    return -1;
}

bool radio_favorites_contains(const radio_station_t *station)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = station->url[0] && index_of(station) >= 0;
    xSemaphoreGive(s_lock);
    return found;
}

bool radio_favorites_add(const radio_station_t *station)
{
    if (!station->url[0] || !s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = index_of(station) < 0 && s_count < RADIO_LIST_MAX;
    if (ok) {
        FILE *f = fopen(RADIO_FAVORITES_FILE, "a");
        ok = f && fprintf(f, "%s | %s%s%s\n", station->name, station->url, station->uuid[0] ? " | " : "",
                          station->uuid) > 0;
        if (f) {
            ok &= (fclose(f) == 0);
        }
        if (ok) {
            s_favorites[s_count++] = *station;
        }
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s: %s", station->name, ok ? "added" : "not added");
    return ok;
}

bool radio_favorites_remove(const radio_station_t *station)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = index_of(station) >= 0;
    char *text = ok ? read_file() : NULL;
    FILE *f = text ? fopen(RADIO_FAVORITES_FILE ".tmp", "w") : NULL;
    ok = (f != NULL);
    // The file written again without its line (comments and other lines kept)
    for (char *line = text; f && line && *line;) {
        char *end = strchr(line, '\n');
        if (end) {
            *end = '\0';
        }
        char copy[RADIO_NAME_SIZE + RADIO_URL_SIZE + RADIO_UUID_SIZE + 16];
        strlcpy(copy, line, sizeof(copy));
        radio_station_t s;
        bool same = parse_line(copy, &s) && (strcmp(s.url, station->url) == 0 ||
                                             (station->uuid[0] && strcmp(s.uuid, station->uuid) == 0));
        if (!same) {
            fprintf(f, "%s\n", line);
        }
        line = end ? end + 1 : NULL;
    }
    free(text);
    if (f) {
        ok = (fclose(f) == 0);
        remove(RADIO_FAVORITES_FILE);
        ok &= (rename(RADIO_FAVORITES_FILE ".tmp", RADIO_FAVORITES_FILE) == 0);
        load();
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s: %s", station->name, ok ? "removed" : "not removed");
    return ok;
}

int radio_favorites_find(const char *norm)
{
    // The text said in the favourite's name ("nostalgie" for "Nostalgie Belgique")
    if (!s_lock) {
        return -1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int found = -1;
    size_t best = 0;
    for (int i = 0; i < s_count; i++) {
        char name[FAV_NORM_SIZE];
        app_text_normalize(s_favorites[i].name, name, sizeof(name));
        size_t len = strlen(norm);
        bool match = len >= 3 && (strstr(name, norm) != NULL || strcmp(name, norm) == 0);
        if (match && (found < 0 || strlen(name) < best)) {
            found = i;          // the shortest name containing what was said: the closest
            best = strlen(name);
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}
