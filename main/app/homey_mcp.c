/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "llm_sse.h"
#include "homey_mcp.h"

#define HOMEY_TIMEOUT_MS        10000
/* The full tool list is large (~190 tools, ~180 KB): read into PSRAM, then only the selection is kept */
#define HOMEY_MAX_BODY          (512 * 1024)
/*
 * Results given to the model, after the useless fields are removed (the room list is ~3,600 characters, the
 * home briefing ~6,600); the conversation keeps a shortened copy once answered (claude_client.c)
 */
#define HOMEY_MAX_RESULT        12000
#define HOMEY_PROTOCOL          "2025-06-18"

static const char *TAG = "homey";

/*
 * Tools offered to the model: everyday home control by voice. Left out: deleting, installing, renaming,
 * editing flows or apps, rebooting, administration (and they would cost ~50,000 tokens per question).
 */
static const char *const s_selected[] = {
    "devices_resolve", "devices_get_state", "devices_set_capability", "devices_zone_set_capability",
    "devices_find", "zones_list", "zones_get_activity", "moods_list", "moods_activate", "flows_list",
    "flows_trigger", "advanced_flows_list", "advanced_flows_trigger", "home_briefing", "users_get_presence",
    "energy_get_live",
};
#define SELECTED_COUNT  (sizeof(s_selected) / sizeof(s_selected[0]))

static char *s_url = NULL;
static char *s_auth = NULL;             /* "Bearer <key>" */
static char s_session[96];              /* Mcp-Session-Id given by the server */
static bool s_available[SELECTED_COUNT];
static int s_next_id = 1;
static SemaphoreHandle_t s_lock = NULL;

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(evt->header_key, "Mcp-Session-Id") == 0) {
        strlcpy(s_session, evt->header_value, sizeof(s_session));
    }
    return ESP_OK;
}

/*
 * POST a JSON-RPC message; the answer (JSON, or an event stream whose data is JSON) is parsed into *out
 * (NULL for a notification). Returns the HTTP status, or -1 on a network error.
 */
static int post(const char *body, cJSON **out)
{
    if (out) {
        *out = NULL;
    }
    esp_http_client_config_t config = {
        .url = s_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HOMEY_TIMEOUT_MS,
        .event_handler = on_http_event,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return -1;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json, text/event-stream");
    esp_http_client_set_header(client, "MCP-Protocol-Version", HOMEY_PROTOCOL);
    if (s_auth) {
        esp_http_client_set_header(client, "Authorization", s_auth);
    }
    if (s_session[0]) {
        esp_http_client_set_header(client, "Mcp-Session-Id", s_session);
    }
    int status = -1;
    size_t len = strlen(body);
    llm_buf_t answer = {0};
    if (esp_http_client_open(client, len) == ESP_OK && esp_http_client_write(client, body, len) == (int)len &&
            esp_http_client_fetch_headers(client) >= 0) {
        status = esp_http_client_get_status_code(client);
        char chunk[1024];
        int n;
        while ((n = esp_http_client_read(client, chunk, sizeof(chunk))) > 0) {
            if (answer.len < HOMEY_MAX_BODY) {
                llm_buf_append(&answer, chunk, n);
            }
        }
    }
    esp_http_client_cleanup(client);
    if (out && answer.len) {
        // An event stream carries the JSON in a "data:" line
        const char *json = answer.p;
        const char *data = strstr(answer.p, "data:");
        if (answer.p[0] != '{' && data) {
            json = data + 5;
        }
        *out = cJSON_Parse(json);
    }
    if (status != 200 && status != 202) {
        ESP_LOGW(TAG, "HTTP %d: %.200s", status, answer.len ? answer.p : "");
    }
    llm_buf_free(&answer);
    return status;
}

static char *request(const char *method, cJSON *params)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(msg, "id", s_next_id++);
    cJSON_AddStringToObject(msg, "method", method);
    if (params) {
        cJSON_AddItemToObject(msg, "params", params);
    }
    char *text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    return text;
}

/* MCP handshake: initialize, then the "initialized" notification. A new session each time. */
static bool session_open(void)
{
    s_session[0] = '\0';
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "protocolVersion", HOMEY_PROTOCOL);
    cJSON_AddObjectToObject(params, "capabilities");
    cJSON *info = cJSON_AddObjectToObject(params, "clientInfo");
    cJSON_AddStringToObject(info, "name", "esp32-s3-box-assistant");
    cJSON_AddStringToObject(info, "version", "1.0");
    char *body = request("initialize", params);
    cJSON *answer = NULL;
    int status = body ? post(body, &answer) : -1;
    free(body);
    const cJSON *server = cJSON_GetObjectItem(cJSON_GetObjectItem(answer, "result"), "serverInfo");
    const cJSON *name = cJSON_GetObjectItem(server, "name");
    bool ok = (status == 200 && cJSON_IsString(name));
    if (ok) {
        ESP_LOGI(TAG, "connected to %s (session %s)", name->valuestring, s_session[0] ? "yes" : "none");
        post("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", NULL);
    }
    cJSON_Delete(answer);
    return ok;
}

static int selected_index(const char *name)
{
    for (size_t i = 0; i < SELECTED_COUNT; i++) {
        if (strcmp(name, s_selected[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

cJSON *homey_mcp_load_tools(const char *url, const char *key)
{
    if (!url || !url[0]) {
        return NULL;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    free(s_url);
    s_url = strdup(url);
    free(s_auth);
    s_auth = NULL;
    if (key && key[0]) {
        asprintf(&s_auth, "Bearer %s", key);
    }
    int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *definitions = NULL;
    if (session_open()) {
        char *body = request("tools/list", NULL);
        cJSON *answer = NULL;
        int status = body ? post(body, &answer) : -1;
        free(body);
        const cJSON *tools = cJSON_GetObjectItem(cJSON_GetObjectItem(answer, "result"), "tools");
        definitions = cJSON_CreateArray();
        const cJSON *tool;
        cJSON_ArrayForEach(tool, tools) {
            const cJSON *name = cJSON_GetObjectItem(tool, "name");
            int i = cJSON_IsString(name) ? selected_index(name->valuestring) : -1;
            if (i < 0) {
                continue;
            }
            // MCP "inputSchema" becomes Claude's "input_schema"
            cJSON *def = cJSON_CreateObject();
            cJSON_AddStringToObject(def, "name", name->valuestring);
            const cJSON *description = cJSON_GetObjectItem(tool, "description");
            cJSON_AddStringToObject(def, "description",
                                    cJSON_IsString(description) ? description->valuestring : name->valuestring);
            const cJSON *schema = cJSON_GetObjectItem(tool, "inputSchema");
            cJSON_AddItemToObject(def, "input_schema",
                                  schema ? cJSON_Duplicate(schema, true) : cJSON_Parse("{\"type\":\"object\"}"));
            cJSON_AddItemToArray(definitions, def);
            s_available[i] = true;
        }
        ESP_LOGI(TAG, "%d of %d tools offered (HTTP %d, %lld ms)", cJSON_GetArraySize(definitions),
                 cJSON_GetArraySize(tools), status, (esp_timer_get_time() - t0) / 1000);
        cJSON_Delete(answer);
        if (cJSON_GetArraySize(definitions) == 0) {
            cJSON_Delete(definitions);
            definitions = NULL;
        }
    } else {
        ESP_LOGW(TAG, "Homey MCP server not reachable at %s", s_url);
    }
    xSemaphoreGive(s_lock);
    return definitions;
}

bool homey_mcp_has_tool(const char *name)
{
    int i = selected_index(name);
    return i >= 0 && s_available[i];
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

/* A JSON text without its useless fields (the text itself if it is not JSON) */
static char *compact_text(char *text)
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

/* Text of a tools/call answer: the text items of "content", without useless fields, shortened if still long */
static char *result_text(const cJSON *result, bool *is_error)
{
    *is_error = cJSON_IsTrue(cJSON_GetObjectItem(result, "isError"));
    llm_buf_t text = {0};
    const cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(result, "content")) {
        const cJSON *t = cJSON_GetObjectItem(item, "text");
        if (cJSON_IsString(t)) {
            if (text.len) {
                llm_buf_append(&text, "\n", 1);
            }
            llm_buf_append(&text, t->valuestring, strlen(t->valuestring));
        }
    }
    char *out = compact_text(text.p ? text.p : strdup("{}"));
    size_t len = strlen(out);
    if (len > HOMEY_MAX_RESULT) {
        ESP_LOGW(TAG, "result of %u characters cut at %d", (unsigned)len, HOMEY_MAX_RESULT);
        out[HOMEY_MAX_RESULT] = '\0';
        char *cut = NULL;
        asprintf(&cut, "%s [... cut]", out);
        if (cut) {
            free(out);
            out = cut;
        }
    }
    return out;
}

char *homey_mcp_call(const char *name, const cJSON *input, bool *is_error)
{
    *is_error = true;
    if (!s_url || !s_lock) {
        return strdup("{\"error\":\"Homey not configured\"}");
    }
    int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    char *out = NULL;
    // The session may have expired (Homey restarted...): then a new one, and the call once more
    for (int attempt = 0; attempt < 2 && !out; attempt++) {
        if (attempt > 0 && !session_open()) {
            break;
        }
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "name", name);
        cJSON_AddItemToObject(params, "arguments", input ? cJSON_Duplicate(input, true) : cJSON_CreateObject());
        char *body = request("tools/call", params);
        cJSON *answer = NULL;
        int status = body ? post(body, &answer) : -1;
        free(body);
        const cJSON *result = cJSON_GetObjectItem(answer, "result");
        if (status == 200 && result) {
            out = result_text(result, is_error);
        } else if (status == 200 && cJSON_GetObjectItem(answer, "error")) {
            const cJSON *message = cJSON_GetObjectItem(cJSON_GetObjectItem(answer, "error"), "message");
            asprintf(&out, "{\"error\":\"%s\"}", cJSON_IsString(message) ? message->valuestring : "error");
        }
        cJSON_Delete(answer);
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s: %s in %lld ms", name, out ? (*is_error ? "error" : "ok") : "unreachable",
             (esp_timer_get_time() - t0) / 1000);
    return out ? out : strdup("{\"error\":\"Homey not reachable\"}");
}
