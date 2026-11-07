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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "llm_sse.h"
#include "debug_trace.h"
#include "settings.h"
#include "tools.h"
#include "homey_mcp.h"
#include "mcp_client.h"

#define MCP_TIMEOUT_MS          10000
/* A full tool list can be large (Homey: ~190 tools, ~180 KB): read into PSRAM, then only the selection is kept */
#define MCP_MAX_BODY            (512 * 1024)
/*
 * Results given to the model (Homey's room list is ~3,600 characters, its home briefing ~6,600), kept whole in the
 * conversation (shortened, the model believed it had made up the rest of a list)
 */
#define MCP_MAX_RESULT          12000
#define MCP_PROTOCOL            "2025-06-18"
#define MCP_MAX_SERVERS         99
/* Tools offered by a server whose tools are not chosen (McpN_tools) */
#define MCP_DEFAULT_MAX_TOOLS   20
/* A server not reachable: tried again after this */
#define MCP_RETRY_S             60
/* The definitions of all the tools sent with each question, above which the traces warn */
#define MCP_TOOLS_WARN_BYTES    (30 * 1024)
/* Pages of tools/list read at most (a server may give its tools in several) */
#define MCP_LIST_PAGES_MAX      10
/* Debug traces: a call, the server's answer and the result given to the model, in pieces of this size (a trace line
   holds 400 characters), at most MCP_TRACE_MAX of each */
#define MCP_TRACE_PIECE         300
#define MCP_TRACE_MAX           (64 * 1024)

static const char *TAG = "mcp";

typedef struct {
    char name[32];
    char on_key[16];            /* "Mcp3_on" */
    char *url;
    char *auth;                 /* "Bearer <key>", NULL without a key */
    char *filter;               /* McpN_tools, NULL when not chosen */
    bool homey;                 /* a Homey Pro: its everyday tools, confirmations, results made shorter */
    volatile bool enabled;
    volatile bool loaded;       /* its tools read */
    volatile bool failed;       /* the last reading failed */
    int retry_s;
    char session[96];           /* Mcp-Session-Id given by the server */
    int next_id;
    SemaphoreHandle_t lock;     /* one request at a time */
    cJSON *definitions;         /* offered (Claude's format, names as the model knows them), set once read */
    cJSON *originals;           /* the server's names of the same tools */
} mcp_server_t;

static mcp_server_t *s_servers = NULL;
static int s_count = 0;
/* A server switched on or off: the tools given to the model again */
static volatile bool s_dirty = false;

/* ---------- Configuration ---------- */

/* Its host ("192.168.1.20", "calendarmcp.googleapis.com"), as a name when none is given */
static void host_of(const char *url, char *out, size_t size)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t n = strcspn(p, ":/?");
    snprintf(out, size, "%.*s", (int)n, p);
}

static void server_add(const char *name, const char *on_key, char *url, char *key, char *filter)
{
    mcp_server_t *s = &s_servers[s_count++];
    if (name && name[0]) {
        strlcpy(s->name, name, sizeof(s->name));
    } else {
        host_of(url, s->name, sizeof(s->name));
    }
    strlcpy(s->on_key, on_key, sizeof(s->on_key));
    s->url = url;
    if (key && key[0]) {
        asprintf(&s->auth, "Bearer %s", key);
    }
    free(key);
    s->filter = filter;
    s->homey = strcasecmp(s->name, "homey") == 0;
    char *on = settings_read_value(on_key);
    s->enabled = !on || !(strcasecmp(on, "off") == 0 || strcmp(on, "0") == 0 || strcasecmp(on, "no") == 0 ||
                          strcasecmp(on, "false") == 0);
    free(on);
    s->lock = xSemaphoreCreateMutex();
    s->next_id = 1;
    ESP_LOGI(TAG, "%s: %s, key: (%d chars), tools: %s, %s", s->name, s->url, s->auth ? (int)strlen(s->auth) - 7 : 0,
             s->filter ? s->filter : "(default)", s->enabled ? "on" : "off");
}

void mcp_init(void)
{
    const sys_param_t *param = settings_get_parameter();
    s_servers = heap_caps_calloc(MCP_MAX_SERVERS + 1, sizeof(mcp_server_t), MALLOC_CAP_SPIRAM);
    if (!s_servers) {
        return;
    }
    homey_mcp_set_confirm_off(param->confirm_off);
    for (int n = 1; n <= MCP_MAX_SERVERS; n++) {
        char key[16];
        snprintf(key, sizeof(key), "Mcp%d_url", n);
        char *url = settings_read_value(key);
        if (!url) {
            continue;
        }
        char name_key[16], key_key[16], tools_key[16], on_key[16];
        snprintf(name_key, sizeof(name_key), "Mcp%d_name", n);
        snprintf(key_key, sizeof(key_key), "Mcp%d_key", n);
        snprintf(tools_key, sizeof(tools_key), "Mcp%d_tools", n);
        snprintf(on_key, sizeof(on_key), "Mcp%d_on", n);
        char *name = settings_read_value(name_key);
        server_add(name, on_key, url, settings_read_value(key_key), settings_read_value(tools_key));
        free(name);
    }
    ESP_LOGI(TAG, "%d MCP server(s)", s_count);
}

int mcp_count(void)
{
    return s_count;
}

const char *mcp_name(int index)
{
    return index >= 0 && index < s_count ? s_servers[index].name : "";
}

mcp_state_t mcp_state(int index, int *tools)
{
    *tools = 0;
    if (index < 0 || index >= s_count) {
        return MCP_STATE_OFF;
    }
    mcp_server_t *s = &s_servers[index];
    if (s->loaded) {
        *tools = cJSON_GetArraySize(s->definitions);
    }
    return !s->enabled ? MCP_STATE_OFF : s->loaded ? MCP_STATE_READY :
           s->failed ? MCP_STATE_UNREACHABLE : MCP_STATE_WAITING;
}

void mcp_set_enabled(int index, bool on)
{
    if (index < 0 || index >= s_count || s_servers[index].enabled == on) {
        return;
    }
    mcp_server_t *s = &s_servers[index];
    s->enabled = on;
    s->retry_s = 0;         // switched on: read at once
    settings_save_str(s->on_key, on ? "on" : "off");
    s_dirty = true;
    ESP_LOGI(TAG, "%s switched %s", s->name, on ? "on" : "off");
}

bool mcp_homey_on(void)
{
    for (int i = 0; i < s_count; i++) {
        if (s_servers[i].homey && s_servers[i].enabled) {
            return true;
        }
    }
    return false;
}

/* ---------- JSON-RPC over HTTP ---------- */

/* A long text in the debug traces (when they are on), cut into pieces that fit their lines */
static void trace_text(const mcp_server_t *s, const char *what, const char *text, size_t len)
{
    if (!debug_trace_on()) {
        return;
    }
    ESP_LOGI(TAG, "[trace] %s %s: %u characters%s", s->name, what, (unsigned)len,
             len > MCP_TRACE_MAX ? " (the beginning only)" : "");
    if (len > MCP_TRACE_MAX) {
        len = MCP_TRACE_MAX;
    }
    for (size_t at = 0; at < len; at += MCP_TRACE_PIECE) {
        size_t n = len - at < MCP_TRACE_PIECE ? len - at : MCP_TRACE_PIECE;
        ESP_LOGI(TAG, "[trace] %s %u| %.*s", what, (unsigned)at, (int)n, text + at);
    }
}

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    mcp_server_t *s = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(evt->header_key, "Mcp-Session-Id") == 0) {
        strlcpy(s->session, evt->header_value, sizeof(s->session));
    }
    return ESP_OK;
}

/*
 * POST a JSON-RPC message; the answer (JSON, or an event stream whose data is JSON) is parsed into *out
 * (NULL for a notification). Returns the HTTP status, or -1 on a network error. traced: the message and the
 * answer go into the debug traces (when they are on).
 */
static int post(mcp_server_t *s, const char *body, cJSON **out, bool traced)
{
    if (out) {
        *out = NULL;
    }
    esp_http_client_config_t config = {
        .url = s->url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = MCP_TIMEOUT_MS,
        .event_handler = on_http_event,
        .user_data = s,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return -1;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json, text/event-stream");
    esp_http_client_set_header(client, "MCP-Protocol-Version", MCP_PROTOCOL);
    if (s->auth) {
        esp_http_client_set_header(client, "Authorization", s->auth);
    }
    if (s->session[0]) {
        esp_http_client_set_header(client, "Mcp-Session-Id", s->session);
    }
    int status = -1;
    size_t len = strlen(body);
    llm_buf_t answer = {0};
    if (traced) {
        trace_text(s, "call", body, len);
    }
    if (esp_http_client_open(client, len) == ESP_OK && esp_http_client_write(client, body, len) == (int)len &&
            esp_http_client_fetch_headers(client) >= 0) {
        status = esp_http_client_get_status_code(client);
        char chunk[1024];
        int n;
        while ((n = esp_http_client_read(client, chunk, sizeof(chunk))) > 0) {
            if (answer.len < MCP_MAX_BODY) {
                llm_buf_append(&answer, chunk, n);
            }
        }
    }
    esp_http_client_cleanup(client);
    if (traced && debug_trace_on()) {
        ESP_LOGI(TAG, "[trace] %s answer: HTTP %d", s->name, status);
        trace_text(s, "answer", answer.p ? answer.p : "", answer.len);
    }
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
        ESP_LOGW(TAG, "%s: HTTP %d: %.200s", s->name, status, answer.len ? answer.p : "");
    }
    llm_buf_free(&answer);
    return status;
}

static char *request(mcp_server_t *s, const char *method, cJSON *params)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(msg, "id", s->next_id++);
    cJSON_AddStringToObject(msg, "method", method);
    if (params) {
        cJSON_AddItemToObject(msg, "params", params);
    }
    char *text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    return text;
}

/* MCP handshake: initialize, then the "initialized" notification. A new session each time. */
static bool session_open(mcp_server_t *s)
{
    s->session[0] = '\0';
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "protocolVersion", MCP_PROTOCOL);
    cJSON_AddObjectToObject(params, "capabilities");
    cJSON *info = cJSON_AddObjectToObject(params, "clientInfo");
    cJSON_AddStringToObject(info, "name", "esp32-s3-box-assistant");
    cJSON_AddStringToObject(info, "version", "1.0");
    char *body = request(s, "initialize", params);
    cJSON *answer = NULL;
    int status = body ? post(s, body, &answer, false) : -1;
    free(body);
    const cJSON *server = cJSON_GetObjectItem(cJSON_GetObjectItem(answer, "result"), "serverInfo");
    const cJSON *name = cJSON_GetObjectItem(server, "name");
    bool ok = (status == 200 && cJSON_IsString(name));
    if (ok) {
        ESP_LOGI(TAG, "%s: connected to %s (session %s)", s->name, name->valuestring, s->session[0] ? "yes" : "none");
        // The server says it is a Homey (whatever its name in CONFIG.INI)
        if (strcasestr(name->valuestring, "homey")) {
            s->homey = true;
        }
        post(s, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", NULL, false);
    }
    cJSON_Delete(answer);
    return ok;
}

/* ---------- Tools offered ---------- */

/* text matches pattern, where '*' stands for any text (case ignored) */
static bool glob_match(const char *pattern, size_t plen, const char *text)
{
    if (plen == 0) {
        return *text == '\0';
    }
    if (*pattern == '*') {
        for (const char *t = text; ; t++) {
            if (glob_match(pattern + 1, plen - 1, t)) {
                return true;
            }
            if (!*t) {
                return false;
            }
        }
    }
    return *text && tolower((unsigned char)*pattern) == tolower((unsigned char)*text) &&
           glob_match(pattern + 1, plen - 1, text + 1);
}

/* The tool chosen by McpN_tools ("list_*, -list_secrets"): kept by a name or pattern, unless one with '-' leaves it out */
static bool filter_keeps(const char *filter, const char *tool)
{
    bool any_include = false, included = false;
    for (const char *p = filter; *p;) {
        p += strspn(p, ", \t;");
        size_t n = strcspn(p, ", \t;");
        if (n == 0) {
            break;
        }
        if (*p == '-') {
            if (glob_match(p + 1, n - 1, tool)) {
                return false;
            }
        } else {
            any_include = true;
            included |= glob_match(p, n, tool);
        }
        p += n;
    }
    return included || !any_include;
}

/* Kept when not chosen: Homey's everyday tools; elsewhere any tool but those deleting or removing */
static bool default_keeps(const mcp_server_t *s, const char *tool)
{
    if (s->homey) {
        return homey_mcp_default_tool(tool);
    }
    return !strcasestr(tool, "delete") && !strcasestr(tool, "remove");
}

/* The name is already the model's: one of the device's tools, or of another server */
static bool name_taken(const char *name, const mcp_server_t *self)
{
    if (tools_is_own(name)) {
        return true;
    }
    for (int i = 0; i < s_count; i++) {
        const mcp_server_t *s = &s_servers[i];
        if (s == self || !s->loaded) {
            continue;
        }
        const cJSON *def;
        cJSON_ArrayForEach(def, s->definitions) {
            if (strcmp(cJSON_GetObjectItem(def, "name")->valuestring, name) == 0) {
                return true;
            }
        }
    }
    return false;
}

/* The name the model knows: the server's, or "<server>_<tool>" when another has it (letters, digits, _ and -) */
static void offered_name(const mcp_server_t *s, const char *tool, char *out, size_t size)
{
    strlcpy(out, tool, size);
    if (!name_taken(out, s)) {
        return;
    }
    snprintf(out, size, "%s_%s", s->name, tool);
    for (char *c = out; *c; c++) {
        if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-') {
            *c = '_';
        }
    }
    ESP_LOGW(TAG, "%s: %s offered as %s (the name is taken)", s->name, tool, out);
}

/* Read the server's tools and keep those offered */
static bool server_load(mcp_server_t *s)
{
    int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(s->lock, portMAX_DELAY);
    bool ok = session_open(s);
    cJSON *definitions = cJSON_CreateArray();
    cJSON *originals = cJSON_CreateArray();
    int total = 0, left_over = 0;
    char *cursor = NULL;
    for (int page = 0; ok && page < MCP_LIST_PAGES_MAX; page++) {
        cJSON *params = NULL;
        if (cursor) {
            params = cJSON_CreateObject();
            cJSON_AddStringToObject(params, "cursor", cursor);
            free(cursor);
            cursor = NULL;
        }
        char *body = request(s, "tools/list", params);
        cJSON *answer = NULL;
        int status = body ? post(s, body, &answer, false) : -1;
        free(body);
        const cJSON *result = cJSON_GetObjectItem(answer, "result");
        if (status != 200 || !result) {
            ok = false;
            cJSON_Delete(answer);
            break;
        }
        const cJSON *tool;
        cJSON_ArrayForEach(tool, cJSON_GetObjectItem(result, "tools")) {
            const cJSON *name = cJSON_GetObjectItem(tool, "name");
            if (!cJSON_IsString(name)) {
                continue;
            }
            total++;
            if (!(s->filter ? filter_keeps(s->filter, name->valuestring) : default_keeps(s, name->valuestring))) {
                continue;
            }
            if (!s->filter && !s->homey && cJSON_GetArraySize(definitions) >= MCP_DEFAULT_MAX_TOOLS) {
                left_over++;
                continue;
            }
            // MCP "inputSchema" becomes Claude's "input_schema"
            char offered[64];
            offered_name(s, name->valuestring, offered, sizeof(offered));
            cJSON *def = cJSON_CreateObject();
            cJSON_AddStringToObject(def, "name", offered);
            const cJSON *description = cJSON_GetObjectItem(tool, "description");
            cJSON_AddStringToObject(def, "description",
                                    cJSON_IsString(description) ? description->valuestring : name->valuestring);
            const cJSON *schema = cJSON_GetObjectItem(tool, "inputSchema");
            cJSON_AddItemToObject(def, "input_schema",
                                  schema ? cJSON_Duplicate(schema, true) : cJSON_Parse("{\"type\":\"object\"}"));
            if (s->homey) {
                homey_mcp_adapt_definition(name->valuestring, def);
            }
            cJSON_AddItemToArray(definitions, def);
            cJSON_AddItemToArray(originals, cJSON_CreateString(name->valuestring));
        }
        const cJSON *next = cJSON_GetObjectItem(result, "nextCursor");
        if (cJSON_IsString(next) && next->valuestring[0]) {
            cursor = strdup(next->valuestring);
        }
        cJSON_Delete(answer);
        if (!cursor) {
            break;
        }
    }
    free(cursor);
    if (ok) {
        s->originals = originals;
        s->definitions = definitions;
        s->loaded = true;
        ESP_LOGI(TAG, "%s: %d of %d tools offered (%lld ms)", s->name, cJSON_GetArraySize(definitions), total,
                 (esp_timer_get_time() - t0) / 1000);
        if (left_over) {
            ESP_LOGW(TAG, "%s: %d more tools left out (at most %d without Mcp_tools: choose them in CONFIG.INI)",
                     s->name, left_over, MCP_DEFAULT_MAX_TOOLS);
        }
    } else {
        cJSON_Delete(definitions);
        cJSON_Delete(originals);
        ESP_LOGW(TAG, "%s: not reachable at %s", s->name, s->url);
    }
    xSemaphoreGive(s->lock);
    return ok;
}

/* The tools of the servers switched on, given to the model with the device's own */
static void tools_offer(void)
{
    cJSON *all = cJSON_CreateArray();
    size_t total = 0, largest = 0;
    const char *largest_name = "";
    for (int i = 0; i < s_count; i++) {
        mcp_server_t *s = &s_servers[i];
        if (!s->enabled || !s->loaded) {
            continue;
        }
        char *text = cJSON_PrintUnformatted(s->definitions);
        size_t size = text ? strlen(text) : 0;
        free(text);
        ESP_LOGI(TAG, "%s: %d tools, %u bytes of definitions", s->name, cJSON_GetArraySize(s->definitions),
                 (unsigned)size);
        total += size;
        if (size > largest) {
            largest = size;
            largest_name = s->name;
        }
        const cJSON *def;
        cJSON_ArrayForEach(def, s->definitions) {
            cJSON_AddItemToArray(all, cJSON_Duplicate(def, true));
        }
    }
    if (total > MCP_TOOLS_WARN_BYTES) {
        ESP_LOGW(TAG, "%u bytes of MCP tool definitions sent with each question (%s: %u): choose fewer tools "
                 "(McpN_tools)", (unsigned)total, largest_name, (unsigned)largest);
    }
    tools_set_added(all);
}

bool mcp_poll(bool wifi_up)
{
    bool changed = false;
    bool offer = false;
    if (s_dirty) {
        s_dirty = false;
        changed = offer = true;
    }
    // One server read per turn (a reading can take up to MCP_TIMEOUT_MS per request)
    for (int i = 0; i < s_count && wifi_up; i++) {
        mcp_server_t *s = &s_servers[i];
        if (!s->enabled || s->loaded || --s->retry_s > 0) {
            continue;
        }
        if (server_load(s)) {
            s->failed = false;
            offer = true;
        } else {
            s->failed = true;
            s->retry_s = MCP_RETRY_S;
        }
        changed = true;
        break;
    }
    if (offer) {
        tools_offer();
    }
    return changed;
}

/* ---------- Calls ---------- */

/* The server switched on that offers this tool, and the server's name of it */
static mcp_server_t *tool_server(const char *name, const char **original)
{
    for (int i = 0; i < s_count; i++) {
        mcp_server_t *s = &s_servers[i];
        if (!s->enabled || !s->loaded) {
            continue;
        }
        int index = 0;
        const cJSON *def;
        cJSON_ArrayForEach(def, s->definitions) {
            if (strcmp(cJSON_GetObjectItem(def, "name")->valuestring, name) == 0) {
                if (original) {
                    *original = cJSON_GetArrayItem(s->originals, index)->valuestring;
                }
                return s;
            }
            index++;
        }
    }
    return NULL;
}

bool mcp_has_tool(const char *name)
{
    return tool_server(name, NULL) != NULL;
}

/* Text of a tools/call answer: the text items of "content", shortened if long */
static char *result_text(const mcp_server_t *s, const cJSON *result, bool *is_error)
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
    char *out = text.p ? text.p : strdup("{}");
    if (s->homey) {
        out = homey_mcp_compact(out);
    }
    size_t len = strlen(out);
    if (len > MCP_MAX_RESULT) {
        ESP_LOGW(TAG, "%s: result of %u characters cut at %d", s->name, (unsigned)len, MCP_MAX_RESULT);
        out[MCP_MAX_RESULT] = '\0';
        char *cut = NULL;
        asprintf(&cut, "%s [... cut]", out);
        if (cut) {
            free(out);
            out = cut;
        }
    }
    return out;
}

/* A call by the server's name of the tool (its lock taken here) */
static char *server_call(void *server, const char *tool, const cJSON *input, bool *is_error)
{
    mcp_server_t *s = server;
    *is_error = true;
    int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(s->lock, portMAX_DELAY);
    char *out = NULL;
    // The session may have expired (the server restarted...): then a new one, and the call once more
    for (int attempt = 0; attempt < 2 && !out; attempt++) {
        if (attempt > 0 && !session_open(s)) {
            break;
        }
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "name", tool);
        cJSON_AddItemToObject(params, "arguments", input ? cJSON_Duplicate(input, true) : cJSON_CreateObject());
        char *body = request(s, "tools/call", params);
        cJSON *answer = NULL;
        int status = body ? post(s, body, &answer, true) : -1;
        free(body);
        const cJSON *result = cJSON_GetObjectItem(answer, "result");
        if (status == 200 && result) {
            out = result_text(s, result, is_error);
        } else if (status == 200 && cJSON_GetObjectItem(answer, "error")) {
            const cJSON *message = cJSON_GetObjectItem(cJSON_GetObjectItem(answer, "error"), "message");
            cJSON *error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", cJSON_IsString(message) ? message->valuestring : "error");
            out = cJSON_PrintUnformatted(error);
            cJSON_Delete(error);
        }
        cJSON_Delete(answer);
    }
    xSemaphoreGive(s->lock);
    ESP_LOGI(TAG, "%s %s: %s in %lld ms", s->name, tool, out ? (*is_error ? "error" : "ok") : "unreachable",
             (esp_timer_get_time() - t0) / 1000);
    if (!out) {
        asprintf(&out, "{\"error\":\"%s not reachable\"}", s->name);
    }
    trace_text(s, "to the model", out, strlen(out));
    return out;
}

char *mcp_call(const char *name, const cJSON *input, bool *is_error)
{
    *is_error = true;
    const char *tool = NULL;
    mcp_server_t *s = tool_server(name, &tool);
    if (!s) {
        return strdup("{\"error\":\"this tool is no longer available\"}");
    }
    cJSON *arguments = input ? cJSON_Duplicate(input, true) : cJSON_CreateObject();
    char *out = NULL;
    if (s->homey) {
        out = homey_mcp_before_call(tool, arguments, server_call, s);
        if (out) {
            *is_error = false;
            trace_text(s, "to the model (by the device)", out, strlen(out));
        }
    }
    if (!out) {
        out = server_call(s, tool, arguments, is_error);
    }
    cJSON_Delete(arguments);
    return out;
}
