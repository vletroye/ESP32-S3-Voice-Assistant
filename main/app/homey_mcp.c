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
#include "homey_mcp.h"

static const char *TAG = "homey";

/*
 * Tools offered to the model when they are not chosen in CONFIG.INI: everyday home control by voice. Left out:
 * deleting, installing, renaming, editing flows or apps, rebooting, administration (and they would cost ~50,000
 * tokens per question: the full list is ~190 tools).
 */
static const char *const s_selected[] = {
    "devices_resolve", "devices_get_state", "devices_set_capability", "devices_zone_set_capability",
    "devices_find", "zones_list", "zones_get_activity", "moods_list", "moods_activate", "flows_list",
    "flows_trigger", "advanced_flows_list", "advanced_flows_trigger", "home_briefing", "users_get_presence",
    "energy_get_live",
};
#define SELECTED_COUNT  (sizeof(s_selected) / sizeof(s_selected[0]))

/* Classes turned off only once confirmed (",socket,heater,"), "" for none */
static char s_confirm_off[72] = "";

/* The tools that turn devices off, given a "confirmed" argument when a confirmation is asked */
#define TOOL_SET            "devices_set_capability"
#define TOOL_ZONE_SET       "devices_zone_set_capability"
#define CONFIRMED_ARG       "confirmed"

bool homey_mcp_default_tool(const char *tool)
{
    for (size_t i = 0; i < SELECTED_COUNT; i++) {
        if (strcmp(tool, s_selected[i]) == 0) {
            return true;
        }
    }
    return false;
}

void homey_mcp_set_confirm_off(const char *classes)
{
    // ",socket,heater," without blanks, lower case: a class is looked for as ",class,"
    char *p = s_confirm_off;
    char *end = s_confirm_off + sizeof(s_confirm_off) - 2;
    *p++ = ',';
    for (const char *c = classes ? classes : ""; *c && p < end; c++) {
        if (*c == ',' || *c == ';') {
            if (p[-1] != ',') {
                *p++ = ',';
            }
        } else if (*c != ' ' && *c != '\t') {
            *p++ = (char)tolower((unsigned char)*c);
        }
    }
    if (p[-1] != ',') {
        *p++ = ',';
    }
    *p = '\0';
    if (strcmp(s_confirm_off, ",") == 0) {
        s_confirm_off[0] = '\0';
    }
    ESP_LOGI(TAG, "Confirmation before turning off: %s", s_confirm_off[0] ? s_confirm_off : "none");
}

static bool class_confirmed_off(const char *cls)
{
    char key[40];
    snprintf(key, sizeof(key), ",%s,", cls);
    for (char *k = key; *k; k++) {
        *k = (char)tolower((unsigned char)*k);
    }
    return s_confirm_off[0] && strstr(s_confirm_off, key) != NULL;
}

void homey_mcp_adapt_definition(const char *tool, cJSON *definition)
{
    cJSON *properties = cJSON_GetObjectItem(cJSON_GetObjectItem(definition, "input_schema"), "properties");
    if (properties && (strcmp(tool, TOOL_SET) == 0 || strcmp(tool, TOOL_ZONE_SET) == 0)) {
        // Turning off some devices (power plugs...) is confirmed first: see confirmation_needed()
        cJSON *confirmed = cJSON_AddObjectToObject(properties, CONFIRMED_ARG);
        cJSON_AddStringToObject(confirmed, "type", "boolean");
        cJSON_AddStringToObject(confirmed, "description",
                                "Set true only when calling again after a confirmation_required answer, once "
                                "the user has explicitly confirmed turning off the devices it named.");
    }
}

/*
 * Fields of no use to answer by voice, removed from JSON results: icons, update dates, the parent of a zone
 * (the nesting already gives it), the folder of a flow. Empty "children" lists go too.
 */
static const char *const s_dropped_fields[] = {
    "icon", "iconObj", "iconOverride", "activeLastUpdated", "lastUpdated", "parent", "folder", NULL
};

static void compact_json(cJSON *node)
{
    cJSON *child = node ? node->child : NULL;
    while (child) {
        cJSON *next = child->next;
        bool drop = false;
        if (child->string) {
            for (const char *const *f = s_dropped_fields; *f && !drop; f++) {
                drop = (strcmp(child->string, *f) == 0);
            }
            drop |= (strcmp(child->string, "children") == 0 && cJSON_IsArray(child) && !child->child);
        }
        if (drop) {
            cJSON_Delete(cJSON_DetachItemViaPointer(node, child));
        } else {
            compact_json(child);
        }
        child = next;
    }
}

char *homey_mcp_compact(char *text)
{
    cJSON *json = (text && (text[0] == '{' || text[0] == '[')) ? cJSON_Parse(text) : NULL;
    if (!json) {
        return text;
    }
    compact_json(json);
    char *compact = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!compact) {
        return text;
    }
    free(text);
    return compact;
}

/* Objects of a result with an id and a class (devices), wherever they are in it */
static void collect_devices(const cJSON *node, cJSON *out)
{
    const cJSON *child;
    if (cJSON_IsObject(node) && cJSON_IsString(cJSON_GetObjectItem(node, "id")) &&
            cJSON_IsString(cJSON_GetObjectItem(node, "class"))) {
        cJSON_AddItemReferenceToArray(out, (cJSON *)node);
    }
    cJSON_ArrayForEach(child, node) {
        if (cJSON_IsObject(child) || cJSON_IsArray(child)) {
            collect_devices(child, out);
        }
    }
}

static bool is_off_value(const cJSON *value)
{
    return cJSON_IsFalse(value) || (cJSON_IsNumber(value) && value->valuedouble == 0) ||
           (cJSON_IsString(value) && (strcasecmp(value->valuestring, "false") == 0 ||
                                      strcasecmp(value->valuestring, "off") == 0));
}

/* The name asked is part of the device's name, or the other way round (case ignored) */
static bool same_name(const char *a, const char *b)
{
    return strcasestr(a, b) != NULL || strcasestr(b, a) != NULL;
}

/*
 * A request turning off devices of a class to confirm (Confirm_off) that are on, not confirmed yet: the answer
 * given to the model instead of doing it (allocated), NULL to go on
 */
static char *confirmation_needed(const char *name, const cJSON *input, homey_mcp_call_t call, void *server)
{
    bool zone = strcmp(name, TOOL_ZONE_SET) == 0;
    if (!s_confirm_off[0] || (!zone && strcmp(name, TOOL_SET) != 0) ||
            cJSON_IsTrue(cJSON_GetObjectItem(input, CONFIRMED_ARG))) {
        return NULL;
    }
    const cJSON *capability = cJSON_GetObjectItem(input, "capability");
    if (!cJSON_IsString(capability) || strcmp(capability->valuestring, "onoff") != 0 ||
            !is_off_value(cJSON_GetObjectItem(input, "value"))) {
        return NULL;
    }
    const cJSON *only = cJSON_GetObjectItem(input, "class");
    if (zone && cJSON_IsString(only) && !class_confirmed_off(only->valuestring)) {
        return NULL;    // "only the lights of the kitchen"
    }
    // The devices that are on now (in the zone), with their class
    cJSON *find = cJSON_CreateObject();
    cJSON_AddStringToObject(find, "capability", "onoff");
    cJSON_AddStringToObject(find, "operator", "truthy");
    cJSON_AddBoolToObject(find, "compact", true);
    const cJSON *zone_id = cJSON_GetObjectItem(input, "zone_id");
    if (zone && cJSON_IsString(zone_id)) {
        cJSON_AddStringToObject(find, "zone_id", zone_id->valuestring);
    }
    bool find_error = false;
    char *found = call(server, "devices_find", find, &find_error);
    cJSON_Delete(find);
    cJSON *result = found && !find_error ? cJSON_Parse(found) : NULL;
    free(found);
    cJSON *devices = cJSON_CreateArray();
    collect_devices(result, devices);

    const cJSON *device_id = cJSON_GetObjectItem(input, "device_id");
    const cJSON *device_name = cJSON_GetObjectItem(input, "name");
    cJSON *names = cJSON_CreateArray();
    const cJSON *d;
    cJSON_ArrayForEach(d, devices) {
        const char *id = cJSON_GetObjectItem(d, "id")->valuestring;
        const char *cls = cJSON_GetObjectItem(d, "class")->valuestring;
        const cJSON *dn = cJSON_GetObjectItem(d, "name");
        bool targeted = zone ||
                        (cJSON_IsString(device_id) && strcmp(device_id->valuestring, id) == 0) ||
                        (!cJSON_IsString(device_id) && cJSON_IsString(device_name) && cJSON_IsString(dn) &&
                         same_name(device_name->valuestring, dn->valuestring));
        if (targeted && class_confirmed_off(cls) && (!zone || !cJSON_IsString(only) ||
                                                     strcasecmp(only->valuestring, cls) == 0)) {
            cJSON_AddItemToArray(names, cJSON_CreateString(cJSON_IsString(dn) ? dn->valuestring : id));
        }
    }
    cJSON_Delete(devices);
    cJSON_Delete(result);
    char *answer = NULL;
    if (cJSON_GetArraySize(names) > 0) {
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddBoolToObject(reply, "confirmation_required", true);
        cJSON_AddItemToObject(reply, "devices", names);
        cJSON_AddStringToObject(reply, "instruction",
                                "Nothing was changed. Briefly ask the user to confirm turning off these devices. "
                                "If they confirm, call this tool again with the same arguments and confirmed=true; "
                                "otherwise do not turn them off.");
        answer = cJSON_PrintUnformatted(reply);
        cJSON_Delete(reply);
        ESP_LOGI(TAG, "%s: confirmation asked before turning off", name);
    } else {
        cJSON_Delete(names);
    }
    return answer;
}

/* ---------- A zone given by its name ("bureau") instead of its id ---------- */

/* The zones of zones_list (id, name) kept this long: asked again after that, or when a name is not found */
#define ZONES_KEEP_US       (10LL * 60 * 1000000)

static cJSON *s_zones = NULL;           /* [{"id","name"}] */
static int64_t s_zones_time = 0;

/* An id of Homey ("9eb2975d-49fd-4b6a-8f1c-2a3b4c5d6e7f"): 36 characters, hexadecimal digits and 4 dashes */
static bool looks_like_id(const char *s)
{
    if (strlen(s) != 36) {
        return false;
    }
    for (int i = 0; i < 36; i++) {
        bool dash = (i == 8 || i == 13 || i == 18 || i == 23);
        if (dash ? s[i] != '-' : !isxdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

/* A name to compare: lower case, without accents (the UTF-8 Latin letters), without blanks around */
static void fold(const char *in, char *out, size_t size)
{
    static const char from[] = "àáâãäåçèéêëìíîïñòóôõöùúûüýÿ";
    static const char to[] = "aaaaaaceeeeiiiinooooouuuuyy";
    size_t n = 0;
    while (*in == ' ') {
        in++;
    }
    for (const unsigned char *p = (const unsigned char *)in; *p && n + 1 < size; p++) {
        unsigned char c = *p;
        if (c == 0xC3 && p[1]) {
            // "é" is 0xC3 0xA9: looked for in from[] (two bytes per letter), lower case first
            unsigned char second = p[1] | 0x20;
            char letter = 0;
            for (size_t i = 0; i + 1 < sizeof(from) - 1; i += 2) {
                if ((unsigned char)from[i + 1] == second) {
                    letter = to[i / 2];
                    break;
                }
            }
            if (letter) {
                out[n++] = letter;
                p++;
                continue;
            }
        }
        out[n++] = (char)tolower(c);
    }
    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }
    out[n] = '\0';
}

/* The objects with an id and a name, wherever they are in the result of zones_list */
static void collect_zones(const cJSON *node, cJSON *out)
{
    const cJSON *id = cJSON_GetObjectItem(node, "id");
    const cJSON *name = cJSON_GetObjectItem(node, "name");
    if (cJSON_IsObject(node) && cJSON_IsString(id) && cJSON_IsString(name)) {
        cJSON *zone = cJSON_CreateObject();
        cJSON_AddStringToObject(zone, "id", id->valuestring);
        cJSON_AddStringToObject(zone, "name", name->valuestring);
        cJSON_AddItemToArray(out, zone);
    }
    const cJSON *child;
    cJSON_ArrayForEach(child, node) {
        if (cJSON_IsObject(child) || cJSON_IsArray(child)) {
            collect_zones(child, out);
        }
    }
}

static void zones_read(homey_mcp_call_t call, void *server)
{
    cJSON *input = cJSON_CreateObject();
    bool error = false;
    char *text = call(server, "zones_list", input, &error);
    cJSON_Delete(input);
    cJSON *result = text && !error ? cJSON_Parse(text) : NULL;
    free(text);
    if (!result) {
        return;
    }
    cJSON_Delete(s_zones);
    s_zones = cJSON_CreateArray();
    collect_zones(result, s_zones);
    cJSON_Delete(result);
    s_zones_time = esp_timer_get_time();
    ESP_LOGI(TAG, "%d zones read", cJSON_GetArraySize(s_zones));
}

/* The id of the zone named so: the same name, else the only one whose name holds it (or is held by it) */
static const char *zone_find(const char *wanted, int *candidates)
{
    char want[64], have[64];
    fold(wanted, want, sizeof(want));
    const char *partial = NULL;
    *candidates = 0;
    const cJSON *zone;
    cJSON_ArrayForEach(zone, s_zones) {
        // Already an id (of a form other than the usual one): kept as it is
        if (strcmp(cJSON_GetObjectItem(zone, "id")->valuestring, wanted) == 0) {
            *candidates = 1;
            return cJSON_GetObjectItem(zone, "id")->valuestring;
        }
        fold(cJSON_GetObjectItem(zone, "name")->valuestring, have, sizeof(have));
        if (strcmp(want, have) == 0) {
            *candidates = 1;
            return cJSON_GetObjectItem(zone, "id")->valuestring;
        }
        if (want[0] && have[0] && (strstr(have, want) || strstr(want, have))) {
            partial = cJSON_GetObjectItem(zone, "id")->valuestring;
            (*candidates)++;
        }
    }
    return *candidates == 1 ? partial : NULL;
}

/*
 * A zone_id that is a name ("bureau", as a model may give it): replaced by the id of the zone of that name; not
 * found, or several, the answer for the model (allocated, with the zones' names), NULL to go on
 */
static char *zone_resolve(cJSON *arguments, homey_mcp_call_t call, void *server)
{
    const cJSON *zone_id = cJSON_GetObjectItem(arguments, "zone_id");
    if (!cJSON_IsString(zone_id) || looks_like_id(zone_id->valuestring)) {
        return NULL;
    }
    char wanted[64];
    strlcpy(wanted, zone_id->valuestring, sizeof(wanted));
    int candidates = 0;
    const char *id = NULL;
    if (s_zones && esp_timer_get_time() - s_zones_time < ZONES_KEEP_US) {
        id = zone_find(wanted, &candidates);
    }
    if (!id) {
        zones_read(call, server);      // not read yet, old, or a zone added since
        id = s_zones ? zone_find(wanted, &candidates) : NULL;
    }
    if (id) {
        ESP_LOGI(TAG, "zone \"%s\" is %s", wanted, id);
        cJSON_ReplaceItemInObject(arguments, "zone_id", cJSON_CreateString(id));
        return NULL;
    }
    ESP_LOGW(TAG, "zone \"%s\": %s", wanted, candidates ? "several match" : "not found");
    cJSON *reply = cJSON_CreateObject();
    char error[128];
    snprintf(error, sizeof(error), "zone_id must be the id of a zone: \"%s\" %s", wanted,
             candidates ? "matches several zones" : "is not the name of any zone");
    cJSON_AddStringToObject(reply, "error", error);
    cJSON *zones = cJSON_AddArrayToObject(reply, "zones");
    const cJSON *zone;
    cJSON_ArrayForEach(zone, s_zones) {
        cJSON_AddItemToArray(zones, cJSON_Duplicate(zone, true));
    }
    char *answer = cJSON_PrintUnformatted(reply);
    cJSON_Delete(reply);
    return answer;
}

char *homey_mcp_before_call(const char *tool, cJSON *arguments, homey_mcp_call_t call, void *server)
{
    char *zone_error = zone_resolve(arguments, call, server);
    if (zone_error) {
        return zone_error;
    }
    char *confirm = confirmation_needed(tool, arguments, call, server);
    cJSON_DeleteItemFromObject(arguments, CONFIRMED_ARG);     // ours, unknown to Homey
    return confirm;
}
