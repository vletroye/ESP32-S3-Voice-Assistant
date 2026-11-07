/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Google Tasks for the model: see google_tasks.h.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "google_auth.h"
#include "google_tasks.h"

static const char *TAG = "gtasks";

#define TASKS_API           "https://tasks.googleapis.com/tasks/v1"
#define NAME_SIZE           96

static const char s_definitions[] =
    "["
    "{\"name\":\"tasks_lists\","
    "\"description\":\"The user's Google Tasks lists (to-do lists, shopping list...), by title. Use it when the user "
    "does not say which list, or the list named is not found.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"tasks_get\","
    "\"description\":\"The open tasks of a Google Tasks list (title, notes, due date), in the list's order. Read them "
    "as a short list (their number first); do not read ids.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"list\":{\"type\":\"string\",\"description\":\"The list's title as the user says it (e.g. 'courses'); the "
    "default list when left out\"},"
    "\"completed\":{\"type\":\"boolean\",\"description\":\"Also the completed tasks\"}}}},"
    "{\"name\":\"tasks_add\","
    "\"description\":\"Add one or several tasks (items) to a Google Tasks list, e.g. the shopping list: one item per "
    "thing ('ajoute du lait et des œufs aux courses' gives 'lait' and 'œufs'). Confirm in a few words.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"list\":{\"type\":\"string\",\"description\":\"The list's title as the user says it; the default list when "
    "left out\"},"
    "\"items\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"The tasks' titles, short, in "
    "the user's language, without the article when it is a thing to buy ('lait', not 'du lait')\"},"
    "\"notes\":{\"type\":\"string\",\"description\":\"A note for a single item, if the user gives details\"},"
    "\"due\":{\"type\":\"string\",\"description\":\"A due date for a single item, YYYY-MM-DD\"},"
    "\"create_list\":{\"type\":\"boolean\",\"description\":\"Create the list when it does not exist (only when the "
    "user asked for a new list, or agreed to it)\"}},"
    "\"required\":[\"items\"]}},"
    "{\"name\":\"tasks_update\","
    "\"description\":\"Change a task of a Google Tasks list, named by its title as the user says it: complete it "
    "('j'ai acheté le lait', 'c'est fait'), reopen it, delete it, or rename it. Several tasks: call it once per task.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"list\":{\"type\":\"string\",\"description\":\"The list's title; the default list when left out\"},"
    "\"task\":{\"type\":\"string\",\"description\":\"The task's title as the user says it\"},"
    "\"action\":{\"type\":\"string\",\"enum\":[\"complete\",\"reopen\",\"delete\",\"rename\"]},"
    "\"new_title\":{\"type\":\"string\",\"description\":\"For rename\"}},"
    "\"required\":[\"task\",\"action\"]}}"
    "]";

const char *google_tasks_definitions(void)
{
    return google_auth_state() == GOOGLE_NOT_CONFIGURED ? NULL : s_definitions;
}

bool google_tasks_has(const char *name)
{
    return strcmp(name, "tasks_lists") == 0 || strcmp(name, "tasks_get") == 0 || strcmp(name, "tasks_add") == 0 ||
           strcmp(name, "tasks_update") == 0;
}

/* ---------- names ---------- */

/* A name to compare: lower case, without accents (the UTF-8 Latin letters), without blanks around */
static void fold(const char *in, char *out, size_t size)
{
    static const char from[] = "àáâãäåçèéêëìíîïñòóôõöùúûüýÿœ";
    static const char to[] = "aaaaaaceeeeiiiinooooouuuuyy";
    size_t n = 0;
    while (*in == ' ') {
        in++;
    }
    for (const unsigned char *p = (const unsigned char *)in; *p && n + 2 < size; p++) {
        if (p[0] == 0xC5 && (p[1] == 0x93 || p[1] == 0x92)) {     // "œ"
            out[n++] = 'o';
            out[n++] = 'e';
            p++;
            continue;
        }
        if (p[0] == 0xC3 && p[1]) {
            unsigned char second = p[1] | 0x20;
            char letter = 0;
            for (size_t i = 0; i + 1 < sizeof(from) - 1 && i / 2 < sizeof(to) - 1; i += 2) {
                if ((unsigned char)from[i] == 0xC3 && (unsigned char)from[i + 1] == second) {
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
        out[n++] = (char)tolower(*p);
    }
    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }
    out[n] = '\0';
}

/*
 * The item of `items` whose "title" is the name: the same (case and accents ignored), else the only one holding it
 * or held by it; NULL when none or several (*several then)
 */
static const cJSON *find_by_title(const cJSON *items, const char *name, bool *several)
{
    char want[NAME_SIZE], have[NAME_SIZE];
    fold(name, want, sizeof(want));
    *several = false;
    const cJSON *partial = NULL;
    int partials = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, items) {
        const cJSON *title = cJSON_GetObjectItem(item, "title");
        if (!cJSON_IsString(title)) {
            continue;
        }
        fold(title->valuestring, have, sizeof(have));
        if (strcmp(want, have) == 0) {
            return item;
        }
        if (want[0] && have[0] && (strstr(have, want) || strstr(want, have))) {
            partial = item;
            partials++;
        }
    }
    *several = partials > 1;
    return partials == 1 ? partial : NULL;
}

/* ---------- API ---------- */

static char *error_json(const char *message)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", message);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return text;
}

/* The answer of a call that failed, for the model */
static char *api_error(int status, const char *response)
{
    return google_api_error(status, response, "Google Tasks");
}

/* The lists ({"items":[{"id","title"}]}), NULL on error (*error then the answer for the model) */
static cJSON *lists_read(char **error)
{
    int status;
    char *response = google_api("GET", TASKS_API "/users/@me/lists?maxResults=100", NULL, &status);
    cJSON *json = status == 200 && response ? cJSON_Parse(response) : NULL;
    if (!json) {
        *error = api_error(status, response);
    }
    free(response);
    return json;
}

/*
 * The list named (the first one, the default list, when name is NULL or ""): its id into id, its title into title;
 * created when asked and missing; false with the answer for the model in *error
 */
static bool list_find(const char *name, bool create, char *id, size_t id_size, char *title, size_t title_size,
                      char **error)
{
    cJSON *lists = lists_read(error);
    if (!lists) {
        return false;
    }
    const cJSON *items = cJSON_GetObjectItem(lists, "items");
    const cJSON *found = NULL;
    bool several = false;
    if (!name || !name[0]) {
        found = cJSON_GetArrayItem(items, 0);
    } else {
        found = find_by_title(items, name, &several);
    }
    bool ok = false;
    if (found) {
        strlcpy(id, cJSON_GetObjectItem(found, "id")->valuestring, id_size);
        strlcpy(title, cJSON_GetObjectItem(found, "title")->valuestring, title_size);
        ok = true;
    } else if (create && !several && name && name[0]) {
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "title", name);
        char *text = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        int status;
        char *response = google_api("POST", TASKS_API "/users/@me/lists", text, &status);
        free(text);
        cJSON *created = status == 200 && response ? cJSON_Parse(response) : NULL;
        const cJSON *new_id = cJSON_GetObjectItem(created, "id");
        if (cJSON_IsString(new_id)) {
            strlcpy(id, new_id->valuestring, id_size);
            strlcpy(title, name, title_size);
            ok = true;
            ESP_LOGI(TAG, "list \"%s\" created", name);
        } else {
            *error = api_error(status, response);
        }
        cJSON_Delete(created);
        free(response);
    } else {
        // Not found: the lists' titles, for the model to ask or choose
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "error", several ? "several lists match this name" : "no list of this name");
        cJSON *titles = cJSON_AddArrayToObject(o, "lists");
        const cJSON *item;
        cJSON_ArrayForEach(item, items) {
            const cJSON *t = cJSON_GetObjectItem(item, "title");
            if (cJSON_IsString(t)) {
                cJSON_AddItemToArray(titles, cJSON_CreateString(t->valuestring));
            }
        }
        *error = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
    }
    cJSON_Delete(lists);
    return ok;
}

/* The tasks of a list ({"items":[...]}), with the completed ones when asked; NULL on error */
static cJSON *tasks_read(const char *list_id, bool completed, char **error)
{
    char url[256];
    snprintf(url, sizeof(url), TASKS_API "/lists/%s/tasks?maxResults=100&showCompleted=%s&showHidden=%s", list_id,
             completed ? "true" : "false", completed ? "true" : "false");
    int status;
    char *response = google_api("GET", url, NULL, &status);
    cJSON *json = status == 200 && response ? cJSON_Parse(response) : NULL;
    if (!json) {
        *error = api_error(status, response);
    }
    free(response);
    return json;
}

static const char *string_input(const cJSON *input, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(input, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

/* ---------- tools ---------- */

static char *run_lists(void)
{
    char *error = NULL;
    cJSON *lists = lists_read(&error);
    if (!lists) {
        return error;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON *titles = cJSON_AddArrayToObject(o, "lists");
    const cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(lists, "items")) {
        const cJSON *t = cJSON_GetObjectItem(item, "title");
        if (cJSON_IsString(t)) {
            cJSON_AddItemToArray(titles, cJSON_CreateString(t->valuestring));
        }
    }
    cJSON_Delete(lists);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return text;
}

static char *run_get(const cJSON *input)
{
    char id[128], title[NAME_SIZE];
    char *error = NULL;
    if (!list_find(string_input(input, "list"), false, id, sizeof(id), title, sizeof(title), &error)) {
        return error;
    }
    bool completed = cJSON_IsTrue(cJSON_GetObjectItem(input, "completed"));
    cJSON *tasks = tasks_read(id, completed, &error);
    if (!tasks) {
        return error;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "list", title);
    cJSON *out = cJSON_AddArrayToObject(o, "tasks");
    const cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(tasks, "items")) {
        const char *t = string_input(item, "title");
        if (!t || !t[0]) {
            continue;
        }
        cJSON *task = cJSON_CreateObject();
        cJSON_AddStringToObject(task, "title", t);
        const char *notes = string_input(item, "notes");
        if (notes && notes[0]) {
            cJSON_AddStringToObject(task, "notes", notes);
        }
        const char *due = string_input(item, "due");
        if (due) {
            char day[11];
            strlcpy(day, due, sizeof(day));     // "2026-11-10T00:00:00.000Z": the day only
            cJSON_AddStringToObject(task, "due", day);
        }
        const char *status = string_input(item, "status");
        if (completed && status && strcmp(status, "completed") == 0) {
            cJSON_AddBoolToObject(task, "completed", true);
        }
        cJSON_AddItemToArray(out, task);
    }
    cJSON_AddNumberToObject(o, "count", cJSON_GetArraySize(out));
    cJSON_Delete(tasks);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return text;
}

static char *run_add(const cJSON *input, bool *is_error)
{
    const cJSON *items = cJSON_GetObjectItem(input, "items");
    if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) == 0) {
        *is_error = true;
        return error_json("give items");
    }
    char id[128], title[NAME_SIZE];
    char *error = NULL;
    if (!list_find(string_input(input, "list"), cJSON_IsTrue(cJSON_GetObjectItem(input, "create_list")), id,
                   sizeof(id), title, sizeof(title), &error)) {
        *is_error = true;
        return error;
    }
    char url[256];
    snprintf(url, sizeof(url), TASKS_API "/lists/%s/tasks", id);
    bool single = cJSON_GetArraySize(items) == 1;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "list", title);
    cJSON *added = cJSON_AddArrayToObject(o, "added");
    cJSON *failed = NULL;
    const cJSON *item;
    cJSON_ArrayForEach(item, items) {
        if (!cJSON_IsString(item) || !item->valuestring[0]) {
            continue;
        }
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "title", item->valuestring);
        const char *notes = string_input(input, "notes");
        const char *due = string_input(input, "due");
        if (single && notes && notes[0]) {
            cJSON_AddStringToObject(body, "notes", notes);
        }
        if (single && due && strlen(due) >= 10) {
            char rfc[32];
            snprintf(rfc, sizeof(rfc), "%.10sT00:00:00.000Z", due);
            cJSON_AddStringToObject(body, "due", rfc);
        }
        char *text = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        int status;
        free(google_api("POST", url, text, &status));
        free(text);
        if (status == 200) {
            cJSON_AddItemToArray(added, cJSON_CreateString(item->valuestring));
        } else {
            if (!failed) {
                failed = cJSON_AddArrayToObject(o, "not_added");
            }
            cJSON_AddItemToArray(failed, cJSON_CreateString(item->valuestring));
            if (status == 401 || status < 0) {
                cJSON_Delete(o);
                *is_error = true;
                return api_error(status, NULL);
            }
        }
    }
    *is_error = cJSON_GetArraySize(added) == 0;
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return text;
}

static char *run_update(const cJSON *input, bool *is_error)
{
    const char *name = string_input(input, "task");
    const char *action = string_input(input, "action");
    if (!name || !action) {
        *is_error = true;
        return error_json("give task and action");
    }
    char id[128], title[NAME_SIZE];
    char *error = NULL;
    if (!list_find(string_input(input, "list"), false, id, sizeof(id), title, sizeof(title), &error)) {
        *is_error = true;
        return error;
    }
    bool reopen = strcmp(action, "reopen") == 0;
    cJSON *tasks = tasks_read(id, reopen, &error);
    if (!tasks) {
        *is_error = true;
        return error;
    }
    bool several = false;
    const cJSON *task = find_by_title(cJSON_GetObjectItem(tasks, "items"), name, &several);
    if (!task) {
        // Not found: the open tasks' titles, for the model to choose or ask
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "error", several ? "several tasks match this title" : "no such task in the list");
        cJSON_AddStringToObject(o, "list", title);
        cJSON *titles = cJSON_AddArrayToObject(o, "tasks");
        const cJSON *item;
        cJSON_ArrayForEach(item, cJSON_GetObjectItem(tasks, "items")) {
            const char *t = string_input(item, "title");
            if (t && t[0]) {
                cJSON_AddItemToArray(titles, cJSON_CreateString(t));
            }
        }
        cJSON_Delete(tasks);
        char *text = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        *is_error = true;
        return text;
    }
    char task_title[NAME_SIZE];
    strlcpy(task_title, string_input(task, "title"), sizeof(task_title));
    char url[320];
    snprintf(url, sizeof(url), TASKS_API "/lists/%s/tasks/%s", id, string_input(task, "id"));
    cJSON_Delete(tasks);

    const char *method = "PATCH";
    cJSON *body = cJSON_CreateObject();
    if (strcmp(action, "complete") == 0) {
        cJSON_AddStringToObject(body, "status", "completed");
    } else if (reopen) {
        cJSON_AddStringToObject(body, "status", "needsAction");
        cJSON_AddNullToObject(body, "completed");
    } else if (strcmp(action, "rename") == 0 && string_input(input, "new_title")) {
        cJSON_AddStringToObject(body, "title", string_input(input, "new_title"));
    } else if (strcmp(action, "delete") == 0) {
        method = "DELETE";
    } else {
        cJSON_Delete(body);
        *is_error = true;
        return error_json("unknown action, or no new_title");
    }
    char *text = strcmp(method, "DELETE") == 0 ? NULL : cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    int status;
    char *response = google_api(method, url, text, &status);
    free(text);
    if (status != 200 && status != 204) {
        *is_error = true;
        char *answer = api_error(status, response);
        free(response);
        return answer;
    }
    free(response);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "list", title);
    cJSON_AddStringToObject(o, "task", task_title);
    cJSON_AddStringToObject(o, "done", action);
    char *answer = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return answer;
}

char *google_tasks_run(const char *name, const cJSON *input, bool *is_error)
{
    *is_error = false;
    char *result = NULL;
    if (strcmp(name, "tasks_lists") == 0) {
        result = run_lists();
    } else if (strcmp(name, "tasks_get") == 0) {
        result = run_get(input);
    } else if (strcmp(name, "tasks_add") == 0) {
        result = run_add(input, is_error);
    } else {
        result = run_update(input, is_error);
    }
    if (result && strncmp(result, "{\"error\"", 8) == 0) {
        *is_error = true;
    }
    return result;
}
