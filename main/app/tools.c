/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "file_lang.h"
#include "app_volume.h"
#include "weather.h"
#include "mcp_client.h"
#include "llm.h"
#include "tools.h"
#include "radio.h"
#include "radio_browser.h"
#include "ai_provider_perplexity.h"
#include "google_tasks.h"
#include "google_calendar.h"
#include "gmail.h"
#include "alarms.h"
#include "alarm_ring.h"
#include "file_phrases.h"
#include "game.h"
#include "google_auth.h"

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
    "\"change\":{\"type\":\"integer\",\"minimum\":-100,\"maximum\":100}}}},"
    "{\"name\":\"play_radio\","
    "\"description\":\"Play an internet radio on the device's speaker, found in the Radio-Browser directory "
    "(the most listened stations first). Give a station name, a genre tag, a country code, or several. Tags are "
    "mostly English, as the directory uses them: jazz, rock, classical, pop, chillout, lounge, 80s, oldies, "
    "electronic, hip-hop, news, talk, kids, christmas... The radio starts once your answer has been spoken: keep "
    "it very short (e.g. 'Je mets du jazz.'). The result gives the station that will play, or that none was "
    "found. For stop, next station, or what is playing, the user has voice commands.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Part of the station name, e.g. 'Nostalgie'\"},"
    "\"tag\":{\"type\":\"string\",\"description\":\"One genre tag, e.g. 'jazz'\"},"
    "\"countrycode\":{\"type\":\"string\",\"description\":\"ISO 3166-1 alpha-2 code, e.g. 'BE'\"}}}},"
    "{\"name\":\"timer_set\","
    "\"description\":\"Start a countdown timer on the device: when it ends, a chime rises from silence, then its label is said; it rings until the user touches the screen or says the wake word. Several timers can run. Keep the answer short (e.g. 'C'est parti pour 10 minutes.').\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"duration_s\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":86400,\"description\":\"Its duration in seconds\"},"
    "\"label\":{\"type\":\"string\",\"description\":\"What it is for, in the user's language, to be said when it ends (e.g. 'les pâtes'); omit if none was given\"}},"
    "\"required\":[\"duration_s\"]}},"
    "{\"name\":\"alarm_set\","
    "\"description\":\"Set an alarm clock at a local date and time, once or repeated. It rings with a chime rising from silence (its label said once loud), or plays a radio station rising from silence when 'radio' is given, until the user touches the screen or says the wake word. To change an alarm, delete it (alarm_delete) and set a new one. Confirm briefly with the day and time.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"time\":{\"type\":\"string\",\"description\":\"Local date and time of the first time it rings, as YYYY-MM-DDTHH:MM (e.g. 2026-11-12T07:30), worked out from the current date and time you were given\"},"
    "\"repeat\":{\"type\":\"string\",\"enum\":[\"none\",\"daily\",\"weekdays\",\"weekend\",\"weekly\",\"monthly\",\"monthly_nth_weekday\",\"monthly_last_weekday\",\"monthly_last_day\",\"yearly\"],\"description\":\"How it repeats (none by default). The rule is taken from 'time': monthly = the same day of the month, monthly_nth_weekday = the same weekday and rank as 'time' (the 2nd Tuesday), monthly_last_weekday = the last such weekday of the month, monthly_last_day = the last day of the month, yearly = the same date every year (birthdays); weekly uses 'days'\"},"
    "\"days\":{\"type\":\"array\",\"items\":{\"type\":\"string\",\"enum\":[\"monday\",\"tuesday\",\"wednesday\",\"thursday\",\"friday\",\"saturday\",\"sunday\"]},\"description\":\"The days of a weekly one\"},"
    "\"label\":{\"type\":\"string\",\"description\":\"Its name, in the user's language, if one was given (e.g. 'réveil', 'médicaments')\"},"
    "\"radio\":{\"type\":\"string\",\"description\":\"A radio station to wake up with, as named by the user (e.g. 'Classic 21')\"}},"
    "\"required\":[\"time\"]}},"
    "{\"name\":\"reminder_set\","
    "\"description\":\"Remind the user of something at a local date and time, once or repeated: a short chime, then the text is said. Confirm briefly with the day and time.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"time\":{\"type\":\"string\",\"description\":\"Local date and time of the first time it rings, as YYYY-MM-DDTHH:MM (e.g. 2026-11-12T07:30), worked out from the current date and time you were given\"},"
    "\"text\":{\"type\":\"string\",\"description\":\"What to remind, in the user's language, said as is (e.g. 'appeler Paul')\"},"
    "\"repeat\":{\"type\":\"string\",\"enum\":[\"none\",\"daily\",\"weekdays\",\"weekend\",\"weekly\",\"monthly\",\"monthly_nth_weekday\",\"monthly_last_weekday\",\"monthly_last_day\",\"yearly\"],\"description\":\"How it repeats (none by default). The rule is taken from 'time': monthly = the same day of the month, monthly_nth_weekday = the same weekday and rank as 'time' (the 2nd Tuesday), monthly_last_weekday = the last such weekday of the month, monthly_last_day = the last day of the month, yearly = the same date every year (birthdays); weekly uses 'days'\"},"
    "\"days\":{\"type\":\"array\",\"items\":{\"type\":\"string\",\"enum\":[\"monday\",\"tuesday\",\"wednesday\",\"thursday\",\"friday\",\"saturday\",\"sunday\"]},\"description\":\"The days of a weekly one\"}},"
    "\"required\":[\"time\",\"text\"]}},"
    "{\"name\":\"alarms_list\","
    "\"description\":\"The timers, alarms and reminders set, by time: id, kind, label, local time (when, weekday), seconds left of a timer, repetition, radio. Use it to answer what is set, and before deleting or changing one.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"kind\":{\"type\":\"string\",\"enum\":[\"timer\",\"alarm\",\"reminder\"],\"description\":\"Only this kind\"}}}},"
    "{\"name\":\"alarm_delete\","
    "\"description\":\"Delete a timer, alarm or reminder by its id (from alarms_list), or all of a kind (all=true; ask the user to confirm first when it deletes several).\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"id\":{\"type\":\"integer\"},"
    "\"all\":{\"type\":\"boolean\"},"
    "\"kind\":{\"type\":\"string\",\"enum\":[\"timer\",\"alarm\",\"reminder\",\"any\"],\"description\":\"With all=true: the kind deleted\"}}}},"
    "{\"name\":\"voice_commands\","
    "\"description\":\"The device's own voice commands, handled on the device without you, with the phrases that "
    "start them in the user's language, and the games between BOX. Always call it, before answering, when the "
    "user asks how to ask for, "
    "start or do something on the device, or which command or phrase to say (radio, favorites, models, voices, "
    "tests, clock, settings, language, memory, games...). Answer with one or two natural phrases to say, never the "
    "pattern syntax; if no command does it, say so (you may do it yourself with your tools).\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"device_guide\","
    "\"description\":\"The user guide of this device (in English: say it in the user's language): what it can do and "
    "how each feature works and is set up, with the device's current state (language, Google linked, MCP servers, "
    "radio). Always call it, before answering, when the user asks what you or the device can do, how something works "
    "or is set up, or for help: never answer that from memory or from your list of tools, which covers only part of "
    "the device (games, memory, voices, languages, configuration... are not among them). Without topic: the list of "
    "topics (with 'overview', everything in short); then call it again with the topic needed. It only describes: it "
    "starts nothing.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"topic\":{\"type\":\"string\",\"description\":\"A topic id from the list (e.g. 'radio')\"}}}},"
    "{\"name\":\"alarm_snooze\","
    "\"description\":\"Ring the timer, alarm or reminder that is ringing or just rang (within 10 minutes) again in some minutes ('encore 5 minutes', 'snooze').\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"minutes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":120,\"description\":\"5 when not said\"}}}}"
    "]";

/*
 * Web search by Perplexity (Sonar), offered to the models whose provider has no search of its own (Mistral,
 * OpenRouter), when a Perplexity key is set
 */
static const char s_web_search_json[] =
    "{\"name\":\"web_search\","
    "\"description\":\"Search the web for current information (news, sports results, schedules, prices, opening "
    "hours, recent events...) and get a short answer with the facts found. Before calling it, say a very short "
    "sentence in the user's language, such as 'Je regarde.'. Never read out web addresses, citations or source "
    "names unless asked.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"query\":{\"type\":\"string\",\"description\":\"The question to search, complete on its own (with the place "
    "and the date when they matter), in the user's language\"}},"
    "\"required\":[\"query\"]}}";

static cJSON *s_definitions = NULL;
/* The same tools, followed by web_search */
static cJSON *s_with_search = NULL;
/* The device's tools followed by the added ones (MCP servers), once there are some */
static cJSON *s_all = NULL;
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

static const char *string_input(const cJSON *input, const char *key);

static const cJSON *own_definitions(void)
{
    if (!s_definitions) {
        s_definitions = cJSON_Parse(s_definitions_json);
        if (!s_definitions) {
            ESP_LOGE(TAG, "Invalid tool definitions");
        }
        // Google Tasks, Calendar and Gmail, when Google is configured in CONFIG.INI (linked or not: not linked, or
        // linked without one of them, their answer says how)
        const char *google[] = { google_tasks_definitions(), google_calendar_definitions(), gmail_definitions() };
        for (size_t g = 0; g < sizeof(google) / sizeof(google[0]); g++) {
            cJSON *list = google[g] ? cJSON_Parse(google[g]) : NULL;
            if (google[g] && !list) {
                ESP_LOGE(TAG, "Invalid Google tool definitions (%u)", (unsigned)g);
            }
            cJSON *tool;
            while (s_definitions && list && (tool = cJSON_DetachItemFromArray(list, 0)) != NULL) {
                cJSON_AddItemToArray(s_definitions, tool);
            }
            cJSON_Delete(list);
        }
    }
    return s_definitions;
}

const cJSON *tools_definitions(void)
{
    return s_all ? s_all : own_definitions();
}

/* tools_definitions() followed by web_search (allocated) */
static cJSON *with_search(void)
{
    cJSON *list = cJSON_Duplicate(tools_definitions(), true);
    cJSON *search = cJSON_Parse(s_web_search_json);
    if (list && search) {
        cJSON_AddItemToArray(list, search);
    } else {
        ESP_LOGE(TAG, "Invalid web_search definition");
        cJSON_Delete(search);
    }
    return list;
}

const cJSON *tools_definitions_with_search(void)
{
    if (!s_with_search) {
        s_with_search = with_search();
    }
    return s_with_search;
}

bool tools_is_own(const char *name)
{
    const cJSON *def;
    cJSON_ArrayForEach(def, own_definitions()) {
        const cJSON *n = cJSON_GetObjectItem(def, "name");
        if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0) {
            return true;
        }
    }
    return strcmp(name, "web_search") == 0;
}

void tools_set_added(cJSON *extra)
{
    cJSON *all = cJSON_Duplicate(own_definitions(), true);
    cJSON *item;
    while ((item = cJSON_DetachItemFromArray(extra, 0)) != NULL) {
        cJSON_AddItemToArray(all, item);
    }
    cJSON_Delete(extra);
    // The new lists given to the model while no answer is being generated; the previous ones can then be freed
    cJSON *old_all = s_all;
    cJSON *old_with_search = s_with_search;
    s_all = all;
    s_with_search = with_search();
    llm_set_tools(all);
    cJSON_Delete(old_all);
    cJSON_Delete(old_with_search);
    ESP_LOGI(TAG, "%d tools offered", cJSON_GetArraySize(all));
}

/* The web search, asked to Perplexity: its answer, short and without sources */
static char *run_web_search(const cJSON *input, bool *is_error)
{
    const char *query = string_input(input, "query");
    if (!query || !query[0]) {
        *is_error = true;
        return strdup("{\"error\":\"give 'query'\"}");
    }
    char *answer = perplexity_search(query);
    if (!answer) {
        *is_error = true;
        return strdup("{\"error\":\"the web search failed\"}");
    }
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "answer", answer);
    free(answer);
    char *text = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    return text;
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

static const char *string_input(const cJSON *input, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(input, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static char *run_play_radio(const cJSON *input, bool *is_error)
{
    char first[RADIO_NAME_SIZE];
    int n = radio_browser_play(string_input(input, "name"), string_input(input, "tag"),
                               string_input(input, "countrycode"), first, sizeof(first));
    char *text = NULL;
    if (n < 0) {
        *is_error = true;
        return strdup("{\"error\":\"the radio directory cannot be reached\"}");
    }
    if (n == 0) {
        return strdup("{\"found\":0,\"note\":\"no station found: try another name, tag or country\"}");
    }
    // The radio plays once the answer has been spoken: back to sleep then, without listening again
    s_sleep_after_reply = true;
    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "found", n);
    cJSON_AddStringToObject(result, "playing", first);
    text = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    return text;
}

/* ---------- Timers, alarms, reminders (alarms.c) ---------- */

/* "2026-11-12T07:30" (local time; seconds and a zone after it ignored), 0 when not understood */
static time_t local_time(const char *text)
{
    struct tm t = {0};
    if (!text || sscanf(text, "%d-%d-%dT%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min) != 5) {
        return 0;
    }
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    t.tm_isdst = -1;
    return mktime(&t);
}

static char *alarm_answer(const alarm_t *a)
{
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddItemToObject(result, "set", alarms_to_json(a));
    char *text = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    return text;
}

static char *alarm_set(alarm_kind_t kind, const cJSON *input, bool *is_error)
{
    alarm_t a = { .kind = kind };
    if (kind == ALARM_KIND_TIMER) {
        const cJSON *duration = cJSON_GetObjectItem(input, "duration_s");
        if (!cJSON_IsNumber(duration) || duration->valueint < 1) {
            *is_error = true;
            return strdup("{\"error\":\"give duration_s\"}");
        }
        a.seconds = duration->valueint;
        a.due = time(NULL) + a.seconds;
    } else {
        a.due = local_time(string_input(input, "time"));
        if (!a.due) {
            *is_error = true;
            return strdup("{\"error\":\"give time as YYYY-MM-DDTHH:MM, local time\"}");
        }
        a.repeat = alarms_repeat_from(string_input(input, "repeat"));
        const cJSON *day;
        cJSON_ArrayForEach(day, cJSON_GetObjectItem(input, "days")) {
            a.days |= cJSON_IsString(day) ? alarms_day_bit(day->valuestring) : 0;
        }
        if (a.days && a.repeat == ALARM_REPEAT_NONE) {
            a.repeat = ALARM_REPEAT_WEEKLY;
        }
    }
    const char *label = string_input(input, kind == ALARM_KIND_REMINDER ? "text" : "label");
    strlcpy(a.label, label ? label : "", sizeof(a.label));
    const char *radio = string_input(input, "radio");
    bool radio_found = false;
    if (kind == ALARM_KIND_ALARM && radio && radio[0]) {
        // The station looked for now, its stream kept: the alarm does not depend on the directory when it rings
        strlcpy(a.radio, radio, sizeof(a.radio));
        radio_station_t *found = calloc(RADIO_BROWSER_MAX, sizeof(radio_station_t));
        int n = found ? radio_browser_search(radio, NULL, NULL, found, RADIO_BROWSER_MAX) : 0;
        if (n > 0) {
            const radio_station_t *best = &found[radio_browser_best(radio, found, n)];
            strlcpy(a.radio, best->name, sizeof(a.radio));
            strlcpy(a.radio_url, best->url, sizeof(a.radio_url));
            radio_found = true;
        }
        free(found);
    }
    if (!alarms_clock_set()) {
        *is_error = true;
        return strdup("{\"error\":\"the device's clock is not set yet (no Internet time): try again in a moment\"}");
    }
    if (!alarms_add(&a)) {
        *is_error = true;
        return strdup("{\"error\":\"that time is past, or too many are set already (50)\"}");
    }
    char *answer = alarm_answer(&a);
    if (a.radio[0] && !radio_found) {
        // Said to the model: looked for again when it rings, the chime otherwise
        char *note = NULL;
        asprintf(&note, "{\"ok\":true,\"note\":\"the radio directory did not find this station now: it will be "
                 "looked for again when the alarm rings, else the chime rings\",\"set\":%s",
                 strstr(answer, "\"set\":") + 6);
        if (note) {
            free(answer);
            answer = note;
        }
    }
    return answer;
}

static int kind_from(const char *name)
{
    return !name ? -1 : strcmp(name, "timer") == 0 ? ALARM_KIND_TIMER : strcmp(name, "alarm") == 0 ?
           ALARM_KIND_ALARM : strcmp(name, "reminder") == 0 ? ALARM_KIND_REMINDER : -1;
}

static char *alarms_list_run(const cJSON *input)
{
    int kind = kind_from(string_input(input, "kind"));
    int count = 0;
    alarm_t *list = alarms_list(&count);
    cJSON *result = cJSON_CreateObject();
    cJSON *items = cJSON_AddArrayToObject(result, "items");
    for (int i = 0; i < count; i++) {
        if (kind < 0 || list[i].kind == (alarm_kind_t)kind) {
            cJSON_AddItemToArray(items, alarms_to_json(&list[i]));
        }
    }
    free(list);
    cJSON_AddNumberToObject(result, "count", cJSON_GetArraySize(items));
    char *text = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    return text;
}

static char *alarm_delete_run(const cJSON *input, bool *is_error)
{
    const cJSON *id = cJSON_GetObjectItem(input, "id");
    char *text = NULL;
    if (cJSON_IsNumber(id)) {
        bool done = alarms_remove(id->valueint);
        *is_error = !done;
        asprintf(&text, done ? "{\"deleted\":%d}" : "{\"error\":\"no item %d: call alarms_list\"}", id->valueint);
        return text;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(input, "all"))) {
        asprintf(&text, "{\"deleted\":%d}", alarms_remove_all(kind_from(string_input(input, "kind"))));
        return text;
    }
    *is_error = true;
    return strdup("{\"error\":\"give id, or all=true\"}");
}

static char *voice_commands_run(void)
{
    char *commands = phrases_help();
    char *games = game_help();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "syntax", "One pattern per line under each [section] (the command, named by the "
                            "section and its # comments): [optional part], (one|of|these). A phrase is said to "
                            "the device after its wake word ('Bonjour ESP'), alone or followed by what it needs.");
    cJSON_AddStringToObject(o, "commands", commands ? commands : "");
    cJSON_AddStringToObject(o, "games", games ? games : "");
    cJSON_AddStringToObject(o, "games_note", "Said to one BOX, the others of the network join; a touch stops it.");
    free(commands);
    free(games);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return text;
}

/* The user guide (main/guide/guide.txt): "[id] summary" lines, each followed by its text */
extern const char s_guide_start[] asm("_binary_guide_txt_start");

/* Where the device stands, added to every answer of device_guide */
static void guide_status(cJSON *o)
{
    cJSON *st = cJSON_AddObjectToObject(o, "status");
    cJSON_AddStringToObject(st, "language", app_lang_get()->code);
    static const char *const google[] = { "not configured (no client in CONFIG.INI)", "configured, account not linked",
                                          "linking", "linked", "link refused: to link again" };
    google_state_t g = google_auth_state();
    cJSON_AddStringToObject(st, "google", (size_t)g < sizeof(google) / sizeof(google[0]) ? google[g] : "?");
    static const char *const states[] = { "off", "connecting", "ready", "unreachable" };
    cJSON *servers = cJSON_AddArrayToObject(st, "mcp_servers");
    for (int i = 0; i < mcp_count(); i++) {
        int tools = 0;
        mcp_state_t s = mcp_state(i, &tools);
        char line[96];
        snprintf(line, sizeof(line), "%s: %s, %d tools", mcp_name(i),
                 (size_t)s < sizeof(states) / sizeof(states[0]) ? states[s] : "?", tools);
        cJSON_AddItemToArray(servers, cJSON_CreateString(line));
    }
    cJSON_AddStringToObject(st, "radio", radio_playing() ? radio_station_name() : "not playing");
    cJSON_AddBoolToObject(st, "deep_thinking", llm_reasoning());
}

static char *device_guide_run(const cJSON *input)
{
    const char *topic = string_input(input, "topic");
    cJSON *o = cJSON_CreateObject();
    char *topics = NULL, *text = NULL;
    size_t topics_size = 0, text_size = 0;
    FILE *list = open_memstream(&topics, &topics_size);
    FILE *body = open_memstream(&text, &text_size);
    bool in_topic = false;
    for (const char *p = s_guide_start; *p;) {
        size_t n = strcspn(p, "\n");
        if (*p == '[') {
            size_t id_len = strcspn(p + 1, "]");
            in_topic = topic && strlen(topic) == id_len && strncasecmp(p + 1, topic, id_len) == 0;
            if (list) {
                fprintf(list, "%.*s\n", (int)n, p);
            }
        } else if (in_topic && *p != '#' && body) {
            fprintf(body, "%.*s\n", (int)n, p);
        }
        p += n + (p[n] == '\n');
    }
    if (list) {
        fclose(list);
    }
    if (body) {
        fclose(body);
    }
    if (text && text[0]) {
        cJSON_AddStringToObject(o, "topic", topic);
        cJSON_AddStringToObject(o, "text", text);
    } else {
        if (topic && topic[0]) {
            cJSON_AddStringToObject(o, "note", "unknown topic: the topics are listed");
        }
        cJSON_AddStringToObject(o, "topics", topics ? topics : "");
    }
    free(topics);
    free(text);
    guide_status(o);
    cJSON_AddStringToObject(o, "rules", "Say it in the user's language, short and spoken (no lists read out as "
                            "such); never say the wake word aloud, call it 'the wake word'; for the exact phrase of "
                            "a command, use voice_commands. This guide starts nothing.");
    char *result = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return result;
}

static char *alarm_snooze_run(const cJSON *input, bool *is_error)
{
    const cJSON *minutes = cJSON_GetObjectItem(input, "minutes");
    int m = cJSON_IsNumber(minutes) ? minutes->valueint : 5;
    if (!alarm_ring_snooze(m)) {
        *is_error = true;
        return strdup("{\"error\":\"nothing rang in the last 10 minutes\"}");
    }
    char *text = NULL;
    asprintf(&text, "{\"ok\":true,\"rings_again_in_minutes\":%d}", m);
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
    } else if (strcmp(name, "play_radio") == 0) {
        result = run_play_radio(input, is_error);
    } else if (strcmp(name, "web_search") == 0) {
        result = run_web_search(input, is_error);
    } else if (strcmp(name, "timer_set") == 0) {
        result = alarm_set(ALARM_KIND_TIMER, input, is_error);
    } else if (strcmp(name, "alarm_set") == 0) {
        result = alarm_set(ALARM_KIND_ALARM, input, is_error);
    } else if (strcmp(name, "reminder_set") == 0) {
        result = alarm_set(ALARM_KIND_REMINDER, input, is_error);
    } else if (strcmp(name, "alarms_list") == 0) {
        result = alarms_list_run(input);
    } else if (strcmp(name, "alarm_delete") == 0) {
        result = alarm_delete_run(input, is_error);
    } else if (strcmp(name, "alarm_snooze") == 0) {
        result = alarm_snooze_run(input, is_error);
    } else if (strcmp(name, "voice_commands") == 0) {
        result = voice_commands_run();
    } else if (strcmp(name, "device_guide") == 0) {
        result = device_guide_run(input);
    } else if (google_tasks_has(name)) {
        result = google_tasks_run(name, input, is_error);
    } else if (google_calendar_has(name)) {
        result = google_calendar_run(name, input, is_error);
    } else if (gmail_has(name)) {
        result = gmail_run(name, input, is_error);
    } else if (mcp_has_tool(name)) {
        result = mcp_call(name, input, is_error);
    } else {
        ESP_LOGW(TAG, "Unknown tool %s", name);
        return NULL;
    }
    ESP_LOGI(TAG, "-> (%u chars) %.300s", result ? (unsigned)strlen(result) : 0, result ? result : "(none)");
    return result;
}
