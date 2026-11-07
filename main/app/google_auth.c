/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * The user's Google account: see google_auth.h.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "settings.h"
#include "file_state.h"
#include "google_auth.h"

static const char *TAG = "google";

#define GOOGLE_TOKEN_FILE   STATE_DIR "/google.json"
#define GOOGLE_AUTH_URL     "https://accounts.google.com/o/oauth2/v2/auth"
#define GOOGLE_TOKEN_URL    "https://oauth2.googleapis.com/token"
#define GOOGLE_REVOKE_URL   "https://oauth2.googleapis.com/revoke"
/* Where Google sends the browser once accepted: a "Desktop app" client may use the loopback address; the page does
   not open (nothing listens on the phone), the code is in the address */
#define GOOGLE_REDIRECT     "http://localhost"
/* The access asked: the tasks (Google Tasks) */
#define GOOGLE_SCOPES       "https://www.googleapis.com/auth/tasks"
#define GOOGLE_HTTP_TIMEOUT 10000
#define GOOGLE_RESPONSE_MAX (64 * 1024)

static char *s_client = NULL;               /* Google_client */
static char *s_secret = NULL;               /* Google_secret */
static char *s_code = NULL;                 /* Google_code to exchange (the address pasted, or the code alone) */
static char *s_refresh = NULL;              /* the refresh token, kept in GOOGLE_TOKEN_FILE */
static char *s_access = NULL;               /* the access token, valid until s_access_until */
static int64_t s_access_until = 0;
static volatile bool s_failed = false;
static volatile bool s_unlink = false;
static SemaphoreHandle_t s_lock = NULL;     /* the tokens (main task: linking; chat task: the tools) */

/* "a b&c" -> "a%20b%26c": a value of a form or of a query */
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

/* "4%2F0Ab..." -> "4/0Ab..." (in place) */
static void url_decode(char *s)
{
    char *o = s;
    for (const char *p = s; *p; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            *o++ = *p == '+' ? ' ' : *p;
        }
    }
    *o = '\0';
}

/*
 * An HTTPS request: the method, a body of this content type (NULL for none), the access token when bearer; the
 * response (allocated, PSRAM) and its status (-1: not reached)
 */
static char *http_request(const char *method, const char *url, const char *content_type, const char *body,
                          const char *bearer, int *status)
{
    *status = -1;
    esp_http_client_method_t m = strcmp(method, "POST") == 0 ? HTTP_METHOD_POST :
                                 strcmp(method, "PATCH") == 0 ? HTTP_METHOD_PATCH :
                                 strcmp(method, "DELETE") == 0 ? HTTP_METHOD_DELETE : HTTP_METHOD_GET;
    esp_http_client_config_t config = {
        .url = url,
        .method = m,
        .timeout_ms = GOOGLE_HTTP_TIMEOUT,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return NULL;
    }
    if (body) {
        esp_http_client_set_header(client, "Content-Type", content_type);
    }
    char *auth = NULL;
    if (bearer) {
        asprintf(&auth, "Bearer %s", bearer);
        esp_http_client_set_header(client, "Authorization", auth ? auth : "");
    }
    char *response = NULL;
    size_t len = body ? strlen(body) : 0;
    if (esp_http_client_open(client, len) == ESP_OK && (!body || esp_http_client_write(client, body, len) == (int)len) &&
            esp_http_client_fetch_headers(client) >= 0) {
        *status = esp_http_client_get_status_code(client);
        response = heap_caps_malloc(GOOGLE_RESPONSE_MAX + 1, MALLOC_CAP_SPIRAM);
        size_t got = 0;
        int n;
        while (response && got < GOOGLE_RESPONSE_MAX &&
                (n = esp_http_client_read(client, response + got, GOOGLE_RESPONSE_MAX - got)) > 0) {
            got += n;
        }
        if (response) {
            response[got] = '\0';
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(auth);
    return response;
}

static char *form_post(const char *url, const char *body, int *status)
{
    return http_request("POST", url, "application/x-www-form-urlencoded", body, NULL, status);
}

static const char *json_string(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItem(object, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static void token_save(void)
{
    FILE *f = fopen(GOOGLE_TOKEN_FILE, "w");
    if (f) {
        fprintf(f, "{\"refresh_token\":\"%s\"}", s_refresh ? s_refresh : "");
        fclose(f);
    }
}

void google_auth_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_client = settings_read_value("Google_client");
    s_secret = settings_read_value("Google_secret");
    s_code = settings_read_value("Google_code");
    FILE *f = fopen(GOOGLE_TOKEN_FILE, "r");
    if (f) {
        char text[600];
        size_t n = fread(text, 1, sizeof(text) - 1, f);
        fclose(f);
        text[n] = '\0';
        cJSON *json = cJSON_Parse(text);
        const char *token = json_string(json, "refresh_token");
        if (token && token[0]) {
            s_refresh = strdup(token);
        }
        cJSON_Delete(json);
    }
    // (the client's secret and the tokens are never logged, as the keys)
    ESP_LOGI(TAG, "client: %s, secret: (%d chars), %s%s", s_client ? "set" : "none",
             s_secret ? (int)strlen(s_secret) : 0, s_refresh ? "linked to a Google account" : "not linked",
             s_code ? ", a Google_code to exchange" : "");
}

google_state_t google_auth_state(void)
{
    if (!s_client || !s_secret) {
        return GOOGLE_NOT_CONFIGURED;
    }
    if (s_refresh && !s_unlink) {
        return GOOGLE_LINKED;
    }
    return s_code ? GOOGLE_LINKING : s_failed ? GOOGLE_FAILED : GOOGLE_NOT_LINKED;
}

void google_auth_consent_url(char *out, size_t size)
{
    if (!s_client) {
        out[0] = '\0';
        return;
    }
    char client[160], scopes[160], redirect[64];
    url_encode(client, sizeof(client), s_client);
    url_encode(scopes, sizeof(scopes), GOOGLE_SCOPES);
    url_encode(redirect, sizeof(redirect), GOOGLE_REDIRECT);
    // offline + consent: a refresh token is given (again, after an unlink)
    snprintf(out, size, GOOGLE_AUTH_URL "?client_id=%s&redirect_uri=%s&response_type=code&scope=%s"
             "&access_type=offline&prompt=consent", client, redirect, scopes);
}

/* The tokens of a token response kept; false if it has no access token */
static bool tokens_take(const char *response)
{
    cJSON *json = cJSON_Parse(response);
    const char *access = json_string(json, "access_token");
    const char *refresh = json_string(json, "refresh_token");
    const cJSON *expires = cJSON_GetObjectItem(json, "expires_in");
    if (access) {
        free(s_access);
        s_access = strdup(access);
        // A minute before Google says, not to use one about to expire
        s_access_until = esp_timer_get_time() + ((cJSON_IsNumber(expires) ? expires->valueint : 3600) - 60) * 1000000LL;
    }
    if (refresh) {
        free(s_refresh);
        s_refresh = strdup(refresh);
        token_save();
    }
    cJSON_Delete(json);
    return access != NULL;
}

/* Google_code exchanged for the tokens: the code taken from the address pasted ("http://localhost/?code=4%2F0A..&scope=..") */
static void code_exchange(void)
{
    char *code = s_code;
    char *start = strstr(code, "code=");
    start = start ? start + 5 : code;
    start[strcspn(start, "& \t\r\n")] = '\0';
    url_decode(start);
    char code_enc[512], client[160], secret[160], redirect[64];
    url_encode(code_enc, sizeof(code_enc), start);
    url_encode(client, sizeof(client), s_client);
    url_encode(secret, sizeof(secret), s_secret);
    url_encode(redirect, sizeof(redirect), GOOGLE_REDIRECT);
    char *body = NULL;
    asprintf(&body, "code=%s&client_id=%s&client_secret=%s&redirect_uri=%s&grant_type=authorization_code",
             code_enc, client, secret, redirect);
    int status = -1;
    char *response = body ? form_post(GOOGLE_TOKEN_URL, body, &status) : NULL;
    free(body);
    if (status < 0) {
        free(response);
        ESP_LOGW(TAG, "Google not reached: the code is tried again");
        return;     // kept: tried again at the next turn
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = status == 200 && response && tokens_take(response) && s_refresh;
    xSemaphoreGive(s_lock);
    if (ok) {
        ESP_LOGI(TAG, "Linked to a Google account");
    } else {
        // "invalid_grant": the code expired (10 minutes), was used already, or is not of this client
        ESP_LOGE(TAG, "Code refused (HTTP %d): %.200s", status, response ? response : "");
    }
    s_failed = !ok;
    free(response);
    free(s_code);
    s_code = NULL;
    settings_erase_key("Google_code");      // used once, whatever the result
}

static void unlink_now(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_refresh) {
        // Revoked at Google too (the device no longer appears among the account's connected apps)
        char token[600], body[700];
        url_encode(token, sizeof(token), s_refresh);
        snprintf(body, sizeof(body), "token=%s", token);
        int status;
        free(form_post(GOOGLE_REVOKE_URL, body, &status));
        ESP_LOGI(TAG, "Unlinked (revoked: HTTP %d)", status);
    }
    free(s_refresh);
    free(s_access);
    s_refresh = NULL;
    s_access = NULL;
    remove(GOOGLE_TOKEN_FILE);
    xSemaphoreGive(s_lock);
    s_unlink = false;
}

void google_auth_poll(bool wifi_up)
{
    if (!s_lock || !wifi_up) {
        return;
    }
    if (s_unlink) {
        unlink_now();
    } else if (s_code && s_client && s_secret) {
        code_exchange();
    }
}

void google_auth_unlink(void)
{
    s_unlink = true;        // done by the main task (it calls Google); not linked from now on
}

/* A valid access token (under s_lock): refreshed when it expired; NULL if not linked or Google refused */
static const char *access_token(void)
{
    if (!s_refresh || s_unlink) {
        return NULL;
    }
    if (s_access && esp_timer_get_time() < s_access_until) {
        return s_access;
    }
    char refresh[600], client[160], secret[160];
    url_encode(refresh, sizeof(refresh), s_refresh);
    url_encode(client, sizeof(client), s_client);
    url_encode(secret, sizeof(secret), s_secret);
    char *body = NULL;
    asprintf(&body, "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token", client, secret, refresh);
    int status = -1;
    char *response = body ? form_post(GOOGLE_TOKEN_URL, body, &status) : NULL;
    free(body);
    bool ok = response && status == 200 && tokens_take(response);
    if (!ok) {
        ESP_LOGW(TAG, "Token not refreshed (HTTP %d): %.200s", status, response ? response : "");
    }
    free(response);
    return ok ? s_access : NULL;
}

char *google_api(const char *method, const char *url, const char *json_body, int *status)
{
    *status = 401;
    if (!s_lock) {
        return NULL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *token = access_token();
    char *bearer = token ? strdup(token) : NULL;
    xSemaphoreGive(s_lock);
    if (!bearer) {
        return NULL;
    }
    int64_t t0 = esp_timer_get_time();
    char *response = http_request(method, url, "application/json", json_body, bearer, status);
    free(bearer);
    ESP_LOGI(TAG, "%s %s: HTTP %d in %lld ms", method, url + (strncmp(url, "https://", 8) == 0 ? 8 : 0), *status,
             (esp_timer_get_time() - t0) / 1000);
    return response;
}
