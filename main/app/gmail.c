/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Gmail for the model: see gmail.h.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "google_auth.h"
#include "gmail.h"

static const char *TAG = "gmail";

#define GMAIL_API           "https://gmail.googleapis.com/gmail/v1/users/me/messages"
#define GMAIL_LIST_MAX      10
#define GMAIL_MODIFY_MAX    20
/* A mail's text given to the model, at most (the rest cut) */
#define GMAIL_TEXT_MAX      4000
#define SERVICE             "Gmail"

static const char s_definitions[] =
    "["
    "{\"name\":\"gmail_list\","
    "\"description\":\"The user's mails, newest first: id, from, subject, date (local), unread, a short extract. By "
    "default the inbox; 'query' takes Gmail's search words: is:unread, from:paul, subject:facture, newer_than:2d, "
    "has:attachment... Say them briefly (sender and subject); never read ids or addresses unless asked.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"query\":{\"type\":\"string\",\"description\":\"Gmail search, 'in:inbox' by default (e.g. 'in:inbox "
    "is:unread')\"},"
    "\"max\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":10,\"description\":\"5 by default\"}}}},"
    "{\"name\":\"gmail_read\","
    "\"description\":\"A mail's whole text (plain), with from, to, subject and date, by its id (from gmail_list). "
    "Summarise it unless the user asks to hear it all.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"id\":{\"type\":\"string\"}},"
    "\"required\":[\"id\"]}},"
    "{\"name\":\"gmail_send\","
    "\"description\":\"Send a mail, or a reply to one (reply_to_id: its id, from gmail_list). 'to' must be a whole "
    "address: never guess it; find it in the user's mails (gmail_list with from:name) and say it to the user. Without "
    "confirmed=true nothing is sent: the mail is given back; read it to the user (to, subject, text) and call again "
    "with confirmed=true only once they agree.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"to\":{\"type\":\"string\",\"description\":\"The address, e.g. paul.dupont@example.com\"},"
    "\"subject\":{\"type\":\"string\",\"description\":\"Left out for a reply: 'Re: ' and its subject\"},"
    "\"text\":{\"type\":\"string\",\"description\":\"The mail's text, in the user's language\"},"
    "\"reply_to_id\":{\"type\":\"string\"},"
    "\"confirmed\":{\"type\":\"boolean\"}},"
    "\"required\":[\"to\",\"text\"]}},"
    "{\"name\":\"gmail_modify\","
    "\"description\":\"Mark mails read or unread, archive them (out of the inbox) or put them in the bin "
    "(recoverable for 30 days), by their ids (from gmail_list). Ask before putting several in the bin.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"ids\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
    "\"action\":{\"type\":\"string\",\"enum\":[\"read\",\"unread\",\"archive\",\"trash\"]}},"
    "\"required\":[\"ids\",\"action\"]}}"
    "]";

const char *gmail_definitions(void)
{
    return google_auth_state() == GOOGLE_NOT_CONFIGURED ? NULL : s_definitions;
}

bool gmail_has(const char *name)
{
    return strcmp(name, "gmail_list") == 0 || strcmp(name, "gmail_read") == 0 || strcmp(name, "gmail_send") == 0 ||
           strcmp(name, "gmail_modify") == 0;
}

/* ---------- helpers ---------- */

static const char *string_input(const cJSON *input, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(input, key);
    return cJSON_IsString(item) && item->valuestring[0] ? item->valuestring : NULL;
}

static char *error_json(const char *message)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", message);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return text;
}

static void url_encode(char *out, size_t size, const char *in)
{
    size_t n = 0;
    for (const unsigned char *c = (const unsigned char *)in; *c && n + 4 < size; c++) {
        if (isalnum(*c) || strchr("-_.~", *c)) {
            out[n++] = (char)*c;
        } else {
            n += snprintf(out + n, size - n, "%%%02X", *c);
        }
    }
    out[n] = '\0';
}

/* Gmail's base64url (no padding) decoded: allocated text, NULL on error */
static char *b64url_decode(const char *in)
{
    size_t len = strlen(in);
    char *std = malloc(len + 4);
    if (!std) {
        return NULL;
    }
    for (size_t i = 0; i < len; i++) {
        std[i] = in[i] == '-' ? '+' : in[i] == '_' ? '/' : in[i];
    }
    while (len % 4) {
        std[len++] = '=';
    }
    std[len] = '\0';
    size_t out_len = 0;
    mbedtls_base64_decode(NULL, 0, &out_len, (const unsigned char *)std, len);
    unsigned char *out = heap_caps_malloc(out_len + 1, MALLOC_CAP_SPIRAM);
    if (out && mbedtls_base64_decode(out, out_len + 1, &out_len, (const unsigned char *)std, len) == 0) {
        out[out_len] = '\0';
    } else {
        free(out);
        out = NULL;
    }
    free(std);
    return (char *)out;
}

/* Bytes in base64url without padding (the raw mail sent): allocated */
static char *b64url_encode(const unsigned char *in, size_t len)
{
    size_t out_len = 0;
    mbedtls_base64_encode(NULL, 0, &out_len, in, len);
    char *out = heap_caps_malloc(out_len + 1, MALLOC_CAP_SPIRAM);
    if (!out || mbedtls_base64_encode((unsigned char *)out, out_len + 1, &out_len, in, len) != 0) {
        free(out);
        return NULL;
    }
    out[out_len] = '\0';
    for (char *c = out; *c; c++) {
        *c = *c == '+' ? '-' : *c == '/' ? '_' : *c;
    }
    while (out_len > 0 && out[out_len - 1] == '=') {
        out[--out_len] = '\0';
    }
    return out;
}

/* "&amp;" "&#39;"... decoded in place (the extracts and the HTML mails) */
static void entities_decode(char *s)
{
    static const struct {
        const char *name;
        char c;
    } names[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&#39;", '\'' },
        { "&nbsp;", ' ' }, { "&apos;", '\'' }
    };
    char *o = s;
    for (const char *p = s; *p;) {
        bool done = false;
        if (*p == '&') {
            for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !done; i++) {
                size_t n = strlen(names[i].name);
                if (strncmp(p, names[i].name, n) == 0) {
                    *o++ = names[i].c;
                    p += n;
                    done = true;
                }
            }
        }
        if (!done) {
            *o++ = *p++;
        }
    }
    *o = '\0';
}

/* An HTML mail made plain in place: no styles nor scripts, no tags (a line break for blocks), entities decoded,
   blank runs shortened */
static void html_to_text(char *s)
{
    char *o = s;
    for (const char *p = s; *p;) {
        if (*p == '<') {
            bool block = strncasecmp(p, "<br", 3) == 0 || strncasecmp(p, "<p", 2) == 0 ||
                         strncasecmp(p, "</p", 3) == 0 || strncasecmp(p, "<div", 4) == 0 ||
                         strncasecmp(p, "<tr", 3) == 0 || strncasecmp(p, "<li", 3) == 0;
            const char *skip_to = NULL;
            if (strncasecmp(p, "<style", 6) == 0) {
                skip_to = strcasestr(p, "</style>");
            } else if (strncasecmp(p, "<script", 7) == 0) {
                skip_to = strcasestr(p, "</script>");
            }
            if (skip_to) {
                p = skip_to;
            }
            const char *end = strchr(p, '>');
            p = end ? end + 1 : p + strlen(p);
            if (block) {
                *o++ = '\n';
            }
            continue;
        }
        *o++ = *p++;
    }
    *o = '\0';
    entities_decode(s);
    // Runs of blanks and of empty lines shortened
    o = s;
    int newlines = 0;
    bool space = false;
    for (const char *p = s; *p; p++) {
        if (*p == '\n' || *p == '\r') {
            if (newlines++ < 2 && o != s) {
                *o++ = '\n';
            }
            space = false;
        } else if (*p == ' ' || *p == '\t') {
            space = !newlines && o != s;
        } else {
            if (space) {
                *o++ = ' ';
            }
            space = false;
            newlines = 0;
            *o++ = *p;
        }
    }
    *o = '\0';
}

/* A header of a message ("From", "Subject"...), NULL if absent */
static const char *header(const cJSON *payload, const char *name)
{
    const cJSON *h;
    cJSON_ArrayForEach(h, cJSON_GetObjectItem(payload, "headers")) {
        const char *n = string_input(h, "name");
        if (n && strcasecmp(n, name) == 0) {
            return string_input(h, "value");
        }
    }
    return NULL;
}

/* "2026-11-10 15:00" from Gmail's internalDate (ms since 1970, a string) */
static void date_text(const cJSON *message, char *out, size_t size)
{
    const char *ms = string_input(message, "internalDate");
    time_t when = ms ? (time_t)(atoll(ms) / 1000) : 0;
    struct tm t;
    localtime_r(&when, &t);
    strftime(out, size, "%Y-%m-%d %H:%M", &t);
}

static bool has_label(const cJSON *message, const char *label)
{
    const cJSON *l;
    cJSON_ArrayForEach(l, cJSON_GetObjectItem(message, "labelIds")) {
        if (cJSON_IsString(l) && strcmp(l->valuestring, label) == 0) {
            return true;
        }
    }
    return false;
}

/* A message of the API, NULL with *error */
static cJSON *message_get(const char *id, const char *format_query, char **error)
{
    char url[512];
    snprintf(url, sizeof(url), GMAIL_API "/%s?%s", id, format_query);
    int status;
    char *response = google_api("GET", url, NULL, &status);
    cJSON *json = status == 200 && response ? cJSON_Parse(response) : NULL;
    if (!json) {
        *error = status == 404 ? error_json("no mail with this id: call gmail_list") :
                 google_api_error(status, response, SERVICE);
    }
    free(response);
    return json;
}

#define METADATA "format=metadata&metadataHeaders=From&metadataHeaders=Subject&metadataHeaders=Date" \
                 "&metadataHeaders=Message-ID&metadataHeaders=To"

/* ---------- tools ---------- */

static char *run_list(const cJSON *input)
{
    const char *query = string_input(input, "query");
    const cJSON *max = cJSON_GetObjectItem(input, "max");
    int n = cJSON_IsNumber(max) ? max->valueint : 5;
    n = n < 1 ? 1 : n > GMAIL_LIST_MAX ? GMAIL_LIST_MAX : n;
    char q[300], url[512];
    url_encode(q, sizeof(q), query ? query : "in:inbox");
    snprintf(url, sizeof(url), GMAIL_API "?q=%s&maxResults=%d", q, n);
    int status;
    char *response = google_api("GET", url, NULL, &status);
    cJSON *list = status == 200 && response ? cJSON_Parse(response) : NULL;
    if (!list) {
        char *error = google_api_error(status, response, SERVICE);
        free(response);
        return error;
    }
    free(response);
    cJSON *o = cJSON_CreateObject();
    cJSON *mails = cJSON_AddArrayToObject(o, "mails");
    const cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(list, "messages")) {
        const char *id = string_input(item, "id");
        char *error = NULL;
        cJSON *m = id ? message_get(id, METADATA "&fields=id,labelIds,snippet,internalDate,payload/headers", &error)
                   : NULL;
        if (!m) {
            free(error);
            continue;
        }
        const cJSON *payload = cJSON_GetObjectItem(m, "payload");
        cJSON *mail = cJSON_CreateObject();
        cJSON_AddStringToObject(mail, "id", id);
        const char *from = header(payload, "From");
        cJSON_AddStringToObject(mail, "from", from ? from : "");
        const char *subject = header(payload, "Subject");
        cJSON_AddStringToObject(mail, "subject", subject ? subject : "");
        char date[24];
        date_text(m, date, sizeof(date));
        cJSON_AddStringToObject(mail, "date", date);
        cJSON_AddBoolToObject(mail, "unread", has_label(m, "UNREAD"));
        const char *snippet = string_input(m, "snippet");
        if (snippet) {
            char *text = strdup(snippet);
            if (text) {
                entities_decode(text);
                cJSON_AddStringToObject(mail, "extract", text);
                free(text);
            }
        }
        cJSON_AddItemToArray(mails, mail);
        cJSON_Delete(m);
    }
    const cJSON *estimate = cJSON_GetObjectItem(list, "resultSizeEstimate");
    if (cJSON_IsNumber(estimate)) {
        cJSON_AddNumberToObject(o, "total_estimate", estimate->valueint);
    }
    cJSON_Delete(list);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return text;
}

/* The text of a message: its text/plain part, else its text/html one made plain (allocated), NULL if none */
static char *body_text(const cJSON *part, bool html)
{
    const char *mime = string_input(part, "mimeType");
    const char *data = string_input(cJSON_GetObjectItem(part, "body"), "data");
    if (mime && data && strcmp(mime, html ? "text/html" : "text/plain") == 0) {
        char *text = b64url_decode(data);
        if (text && html) {
            html_to_text(text);
        }
        return text;
    }
    const cJSON *child;
    cJSON_ArrayForEach(child, cJSON_GetObjectItem(part, "parts")) {
        char *text = body_text(child, html);
        if (text) {
            return text;
        }
    }
    return NULL;
}

static char *run_read(const cJSON *input)
{
    const char *id = string_input(input, "id");
    if (!id) {
        return error_json("give id (from gmail_list)");
    }
    char *error = NULL;
    cJSON *m = message_get(id, "format=full", &error);
    if (!m) {
        return error;
    }
    const cJSON *payload = cJSON_GetObjectItem(m, "payload");
    char *text = body_text(payload, false);
    if (!text) {
        text = body_text(payload, true);
    }
    cJSON *o = cJSON_CreateObject();
    const char *names[] = { "From", "To", "Subject" };
    const char *keys[] = { "from", "to", "subject" };
    for (int i = 0; i < 3; i++) {
        const char *value = header(payload, names[i]);
        cJSON_AddStringToObject(o, keys[i], value ? value : "");
    }
    char date[24];
    date_text(m, date, sizeof(date));
    cJSON_AddStringToObject(o, "date", date);
    if (text) {
        size_t len = strlen(text);
        if (len > GMAIL_TEXT_MAX) {
            size_t cut = GMAIL_TEXT_MAX;
            while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80) {
                cut--;      // not in the middle of a UTF-8 character
            }
            text[cut] = '\0';
            cJSON_AddBoolToObject(o, "cut", true);
        }
        cJSON_AddStringToObject(o, "text", text);
        free(text);
    } else {
        cJSON_AddStringToObject(o, "text", "(no text: attachments or images only)");
    }
    cJSON_Delete(m);
    char *answer = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return answer;
}

/* "=?UTF-8?B?...?=": a header in UTF-8 (allocated) */
static char *header_utf8(const char *value)
{
    size_t out_len = 0;
    mbedtls_base64_encode(NULL, 0, &out_len, (const unsigned char *)value, strlen(value));
    char *b64 = malloc(out_len + 1);
    if (!b64 || mbedtls_base64_encode((unsigned char *)b64, out_len + 1, &out_len, (const unsigned char *)value,
                                      strlen(value)) != 0) {
        free(b64);
        return NULL;
    }
    b64[out_len] = '\0';
    char *out = NULL;
    asprintf(&out, "=?UTF-8?B?%s?=", b64);
    free(b64);
    return out;
}

static char *run_send(const cJSON *input, bool *is_error)
{
    const char *to = string_input(input, "to");
    const char *text = string_input(input, "text");
    const char *reply_id = string_input(input, "reply_to_id");
    if (!to || !strchr(to, '@') || !strchr(strchr(to, '@'), '.') || !text) {
        *is_error = true;
        return error_json("give to as a whole address (find it with gmail_list, from:name; never guess it) and text");
    }
    // A reply: its thread, and the headers that keep it there
    char subject[300] = "";
    char message_id[300] = "";
    char thread[64] = "";
    if (reply_id) {
        char *error = NULL;
        cJSON *m = message_get(reply_id, METADATA "&fields=threadId,payload/headers", &error);
        if (!m) {
            *is_error = true;
            return error;
        }
        const cJSON *payload = cJSON_GetObjectItem(m, "payload");
        const char *s = header(payload, "Subject");
        snprintf(subject, sizeof(subject), "%s%s", s && strncasecmp(s, "Re:", 3) == 0 ? "" : "Re: ", s ? s : "");
        const char *mid = header(payload, "Message-ID");
        strlcpy(message_id, mid ? mid : "", sizeof(message_id));
        const char *t = string_input(m, "threadId");
        strlcpy(thread, t ? t : "", sizeof(thread));
        cJSON_Delete(m);
    }
    if (string_input(input, "subject")) {
        strlcpy(subject, string_input(input, "subject"), sizeof(subject));
    }
    if (!cJSON_IsTrue(cJSON_GetObjectItem(input, "confirmed"))) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "confirmation_required", true);
        cJSON_AddStringToObject(o, "to", to);
        cJSON_AddStringToObject(o, "subject", subject);
        cJSON_AddStringToObject(o, "text", text);
        cJSON_AddStringToObject(o, "instruction", "Nothing was sent. Read this mail to the user (to, subject, text) "
                                "and ask them to confirm; call again with confirmed=true only if they agree.");
        char *answer = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        return answer;
    }
    // The mail (RFC 5322, UTF-8 text), then base64url as Gmail wants it
    char *subject_header = subject[0] ? header_utf8(subject) : NULL;
    char *mail = NULL;
    asprintf(&mail, "To: %s\r\nSubject: %s\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\n"
             "Content-Transfer-Encoding: 8bit\r\n%s%s%s%s%s\r\n%s\r\n", to, subject_header ? subject_header : "",
             message_id[0] ? "In-Reply-To: " : "", message_id, message_id[0] ? "\r\nReferences: " : "",
             message_id, message_id[0] ? "\r\n" : "", text);
    free(subject_header);
    char *raw = mail ? b64url_encode((const unsigned char *)mail, strlen(mail)) : NULL;
    free(mail);
    if (!raw) {
        *is_error = true;
        return error_json("the mail could not be made (memory)");
    }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "raw", raw);
    free(raw);
    if (thread[0]) {
        cJSON_AddStringToObject(body, "threadId", thread);
    }
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    int status;
    char *response = google_api("POST", GMAIL_API "/send", json, &status);
    free(json);
    char *answer;
    if (status == 200) {
        answer = strdup("{\"done\":\"sent\"}");
        ESP_LOGI(TAG, "mail sent%s", thread[0] ? " (a reply)" : "");
    } else {
        *is_error = true;
        answer = google_api_error(status, response, SERVICE);
    }
    free(response);
    return answer;
}

static char *run_modify(const cJSON *input, bool *is_error)
{
    const cJSON *ids = cJSON_GetObjectItem(input, "ids");
    const char *action = string_input(input, "action");
    if (!cJSON_IsArray(ids) || !cJSON_GetArraySize(ids) || !action) {
        *is_error = true;
        return error_json("give ids (from gmail_list) and action");
    }
    bool trash = strcmp(action, "trash") == 0;
    const char *body = strcmp(action, "read") == 0 ? "{\"removeLabelIds\":[\"UNREAD\"]}" :
                       strcmp(action, "unread") == 0 ? "{\"addLabelIds\":[\"UNREAD\"]}" :
                       strcmp(action, "archive") == 0 ? "{\"removeLabelIds\":[\"INBOX\"]}" : NULL;
    if (!trash && !body) {
        *is_error = true;
        return error_json("action: read, unread, archive or trash");
    }
    int done = 0, count = 0;
    char *error = NULL;
    const cJSON *id;
    cJSON_ArrayForEach(id, ids) {
        if (!cJSON_IsString(id) || count++ >= GMAIL_MODIFY_MAX) {
            continue;
        }
        char url[256];
        snprintf(url, sizeof(url), GMAIL_API "/%s/%s", id->valuestring, trash ? "trash" : "modify");
        int status;
        char *response = google_api("POST", url, trash ? "{}" : body, &status);
        if (status == 200) {
            done++;
        } else if (!error) {
            error = google_api_error(status, response, SERVICE);
        }
        free(response);
    }
    if (!done && error) {
        *is_error = true;
        return error;
    }
    free(error);
    char *answer = NULL;
    asprintf(&answer, "{\"done\":\"%s\",\"mails\":%d}", action, done);
    ESP_LOGI(TAG, "%d mail(s): %s", done, action);
    return answer;
}

char *gmail_run(const char *name, const cJSON *input, bool *is_error)
{
    *is_error = false;
    char *result;
    if (strcmp(name, "gmail_list") == 0) {
        result = run_list(input);
    } else if (strcmp(name, "gmail_read") == 0) {
        result = run_read(input);
    } else if (strcmp(name, "gmail_send") == 0) {
        result = run_send(input, is_error);
    } else {
        result = run_modify(input, is_error);
    }
    if (result && strncmp(result, "{\"error\"", 8) == 0) {
        *is_error = true;
    }
    return result;
}
