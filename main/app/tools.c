/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "app_lang.h"
#include "app_volume.h"
#include "weather.h"
#include "tools.h"

static const char *TAG = "tools";

static const char s_definitions_json[] =
    "["
    "{\"name\":\"get_weather\","
    "\"description\":\"Current weather and daily forecast (up to 7 days) for a place, from Open-Meteo. "
    "If the user does not name a place, use where the user lives when you know it (e.g. from your "
    "instructions), otherwise ask. Temperatures are in degrees Celsius.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"location\":{\"type\":\"string\",\"description\":\"City name, optionally followed by a comma and the "
    "country, e.g. 'Namur, Belgique'\"},"
    "\"days\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":7,\"description\":\"Days of forecast, starting "
    "today: 1 for today, 2 to include tomorrow...\"}},"
    "\"required\":[\"location\"]}},"
    "{\"name\":\"set_volume\","
    "\"description\":\"Change the speaker volume (0 to 100). Use 'change' for relative requests (about +15 "
    "for 'louder', -15 for 'quieter', more for 'much louder') and 'level' for an exact level. The result gives "
    "the previous and new levels and whether the new volume is already applied (else it applies right after "
    "your answer). Keep the answer short.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"level\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100},"
    "\"change\":{\"type\":\"integer\",\"minimum\":-100,\"maximum\":100}}}}"
    "]";

static cJSON *s_definitions = NULL;
/* Set by a tool whose answer ends the exchange (no follow-up listening) */
static volatile bool s_sleep_after_reply = false;

void tools_reset_requests(void)
{
    s_sleep_after_reply = false;
}

bool tools_sleep_after_reply(void)
{
    return s_sleep_after_reply;
}

const cJSON *tools_definitions(void)
{
    if (!s_definitions) {
        s_definitions = cJSON_Parse(s_definitions_json);
        if (!s_definitions) {
            ESP_LOGE(TAG, "Invalid tool definitions");
        }
    }
    return s_definitions;
}

static char *run_set_volume(const cJSON *input, bool *is_error)
{
    int previous = app_volume_get();
    const cJSON *level = cJSON_GetObjectItem(input, "level");
    const cJSON *change = cJSON_GetObjectItem(input, "change");
    int target = previous;
    if (cJSON_IsNumber(level)) {
        target = level->valueint;
    } else if (cJSON_IsNumber(change)) {
        target = previous + change->valueint;
    } else {
        *is_error = true;
        return strdup("{\"error\":\"give 'level' or 'change'\"}");
    }
    int applied = app_volume_request(target);
    // A volume change ends the exchange: back to sleep after the confirmation
    s_sleep_after_reply = true;
    bool now = (app_volume_get() == applied && !app_volume_pending());
    char *text = NULL;
    asprintf(&text, "{\"previous_level\":%d,\"new_level\":%d,\"applied_now\":%s%s}", previous, applied,
             now ? "true" : "false", applied != target ? ",\"note\":\"limited to 0..100\"" : "");
    return text;
}

char *tools_run(const char *name, const cJSON *input, bool *is_error)
{
    *is_error = false;
    char *input_text = cJSON_PrintUnformatted(input);
    ESP_LOGI(TAG, "%s(%s)", name, input_text ? input_text : "");
    free(input_text);

    char *result = NULL;
    if (strcmp(name, "get_weather") == 0) {
        const cJSON *location = cJSON_GetObjectItem(input, "location");
        const cJSON *days = cJSON_GetObjectItem(input, "days");
        result = weather_get(cJSON_IsString(location) ? location->valuestring : NULL,
                             cJSON_IsNumber(days) ? days->valueint : 1, app_lang_get()->code, is_error);
    } else if (strcmp(name, "set_volume") == 0) {
        result = run_set_volume(input, is_error);
    } else {
        ESP_LOGW(TAG, "Unknown tool %s", name);
        return NULL;
    }
    ESP_LOGI(TAG, "-> %.300s", result ? result : "(none)");
    return result;
}
