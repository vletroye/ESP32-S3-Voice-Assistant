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

char *homey_mcp_before_call(const char *tool, cJSON *arguments, homey_mcp_call_t call, void *server)
{
    char *confirm = confirmation_needed(tool, arguments, call, server);
    cJSON_DeleteItemFromObject(arguments, CONFIRMED_ARG);     // ours, unknown to Homey
    return confirm;
}
