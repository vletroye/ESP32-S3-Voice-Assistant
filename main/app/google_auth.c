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
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "settings.h"
#include "file_state.h"
#include "google_auth.h"

static const char *TAG = "google";

#define GOOGLE_TOKEN_FILE   STATE_DIR "/google.json"
#define GOOGLE_LINK_FILE    "/spiffs/google.htm"        /* GOOGLE.HTM of the configuration drive */
#define GOOGLE_AUTH_URL     "https://accounts.google.com/o/oauth2/v2/auth"
#define GOOGLE_TOKEN_URL    "https://oauth2.googleapis.com/token"
#define GOOGLE_REVOKE_URL   "https://oauth2.googleapis.com/revoke"
/* Where Google sends the browser once accepted: a "Desktop app" client may use the loopback address; the page does
   not open (nothing listens on the phone), the code is in the address */
#define GOOGLE_REDIRECT     "http://127.0.0.1"
/* PKCE: the secret whose hash goes with the consent page's address, given back with the code (kept across the
   restart between the two: the consent is done in configuration mode) */
#define GOOGLE_PKCE_FILE    STATE_DIR "/google_pkce.txt"
#define PKCE_VERIFIER_SIZE  64
/* The access asked: the tasks (Google Tasks), the mails (read, send, mark, archive, to the bin: Gmail), the events
   (Google Calendar). An account linked before one was added gets "insufficient scopes" from it: to link again */
#define GOOGLE_SCOPES       "https://www.googleapis.com/auth/tasks https://www.googleapis.com/auth/gmail.modify " \
                            "https://www.googleapis.com/auth/calendar.events"
#define GOOGLE_HTTP_TIMEOUT 10000
#define GOOGLE_RESPONSE_MAX (256 * 1024)     /* a mail in HTML can be large (PSRAM, freed after) */

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

/* Bytes in base64url, without padding (PKCE) */
static void base64url(const unsigned char *in, size_t len, char *out, size_t size)
{
    size_t n = 0;
    mbedtls_base64_encode((unsigned char *)out, size, &n, in, len);
    out[n] = '\0';
    for (char *c = out; *c; c++) {
        *c = *c == '+' ? '-' : *c == '/' ? '_' : *c;
    }
    while (n > 0 && out[n - 1] == '=') {
        out[--n] = '\0';
    }
}

/* A new PKCE verifier (43 characters from 32 random bytes), kept for the exchange */
static void pkce_new(char *verifier, size_t size)
{
    unsigned char random[32];
    esp_fill_random(random, sizeof(random));
    base64url(random, sizeof(random), verifier, size);
    FILE *f = fopen(GOOGLE_PKCE_FILE, "w");
    if (f) {
        fputs(verifier, f);
        fclose(f);
    }
}

/* The verifier kept, "" if none */
static void pkce_read(char *verifier, size_t size)
{
    verifier[0] = '\0';
    FILE *f = fopen(GOOGLE_PKCE_FILE, "r");
    if (f) {
        if (fgets(verifier, size, f)) {
            verifier[strcspn(verifier, "\r\n")] = '\0';
        }
        fclose(f);
    }
}

/* The consent page's address, its PKCE challenge made from verifier */
static void consent_url(const char *verifier, char *out, size_t size)
{
    if (!s_client) {
        out[0] = '\0';
        return;
    }
    unsigned char hash[32];
    mbedtls_sha256((const unsigned char *)verifier, strlen(verifier), hash, 0);
    char challenge[48];
    base64url(hash, sizeof(hash), challenge, sizeof(challenge));
    // Static, as the other large buffers here: the main task's stack is small (6.5 KB), and these run deep in it
    EXT_RAM_BSS_ATTR static char client[160], scopes[400], redirect[64];
    url_encode(client, sizeof(client), s_client);
    url_encode(scopes, sizeof(scopes), GOOGLE_SCOPES);
    url_encode(redirect, sizeof(redirect), GOOGLE_REDIRECT);
    // offline + consent: a refresh token is given (again, after an unlink)
    snprintf(out, size, GOOGLE_AUTH_URL "?client_id=%s&redirect_uri=%s&response_type=code&scope=%s"
             "&access_type=offline&prompt=consent&code_challenge=%s&code_challenge_method=S256", client, redirect,
             scopes, challenge);
}

void google_auth_consent_url(char *out, size_t size)
{
    char verifier[PKCE_VERIFIER_SIZE];
    pkce_read(verifier, sizeof(verifier));
    if (!verifier[0]) {
        pkce_new(verifier, sizeof(verifier));
    }
    consent_url(verifier, out, size);
}

void google_auth_export_link(void)
{
    google_state_t state = google_auth_state();
    if (state == GOOGLE_NOT_CONFIGURED || state == GOOGLE_LINKED) {
        remove(GOOGLE_LINK_FILE);
        return;
    }
    // Google_code ready to fill in CONFIG.INI (empty)
    char *code = settings_read_value("Google_code");
    if (!code) {
        settings_save_str("Google_code", "");
    }
    free(code);
    // A new verifier for each new page (the code of an older one is refused)
    EXT_RAM_BSS_ATTR static char verifier[PKCE_VERIFIER_SIZE], url[1000], html_url[1200];
    pkce_new(verifier, sizeof(verifier));
    consent_url(verifier, url, sizeof(url));
    // "&" written "&amp;" in the attribute
    size_t n = 0;
    for (const char *c = url; *c && n + 6 < sizeof(html_url); c++) {
        n += *c == '&' ? (size_t)snprintf(html_url + n, sizeof(html_url) - n, "&amp;") : (html_url[n] = *c, 1);
    }
    html_url[n] = '\0';
    FILE *f = fopen(GOOGLE_LINK_FILE, "w");
    if (!f) {
        return;
    }
    // Opened by a double click on the PC: straight to Google's consent page
    fprintf(f, "<!doctype html><html><head><meta charset=\"utf-8\"><meta http-equiv=\"refresh\" content=\"0; "
            "url=%s\"><title>Google</title></head><body><p><a href=\"%s\">Google</a></p><p>Google_code = "
            "http://127.0.0.1/?code=... (CONFIG.INI)</p></body></html>\n", html_url, html_url);
    fclose(f);
    ESP_LOGI(TAG, "GOOGLE.HTM written (the account to link)");
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

/* Google_code exchanged for the tokens: the code taken from the address pasted ("http://127.0.0.1/?code=4%2F0A..&scope=..") */
static void code_exchange(void)
{
    char *code = s_code;
    char *start = strstr(code, "code=");
    start = start ? start + 5 : code;
    start[strcspn(start, "& \t\r\n")] = '\0';
    url_decode(start);
    EXT_RAM_BSS_ATTR static char code_enc[512], client[160], secret[160], redirect[64];
    url_encode(code_enc, sizeof(code_enc), start);
    url_encode(client, sizeof(client), s_client);
    url_encode(secret, sizeof(secret), s_secret);
    url_encode(redirect, sizeof(redirect), GOOGLE_REDIRECT);
    char verifier[PKCE_VERIFIER_SIZE];
    pkce_read(verifier, sizeof(verifier));
    char *body = NULL;
    asprintf(&body, "code=%s&client_id=%s&client_secret=%s&redirect_uri=%s&grant_type=authorization_code"
             "&code_verifier=%s", code_enc, client, secret, redirect, verifier);
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
    // Used once, whatever the result: emptied, its line kept in CONFIG.INI for the next time
    settings_save_str("Google_code", "");
    if (ok) {
        remove(GOOGLE_PKCE_FILE);
    }
}

static void unlink_now(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_refresh) {
        // Revoked at Google too (the device no longer appears among the account's connected apps)
        EXT_RAM_BSS_ATTR static char token[600], body[700];
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
    EXT_RAM_BSS_ATTR static char refresh[600], client[160], secret[160];      // (under s_lock)
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

char *google_api_error(int status, const char *response, const char *service)
{
    char message[300];
    if (status == 401) {
        snprintf(message, sizeof(message), "the Google account is not linked (or the access was withdrawn): the user "
                 "links it on the Settings screen, Paramètres page, Google, Connect");
    } else if (status == 403 && response && (strstr(response, "SCOPE_INSUFFICIENT") ||
               strstr(response, "insufficientPermissions") || strstr(response, "insufficient authentication scopes"))) {
        snprintf(message, sizeof(message), "the Google account was linked without access to %s: the user links it "
                 "again (Settings screen, Paramètres page, Google: Disconnect, then Connect)", service);
    } else if (status == 403 && response && (strstr(response, "SERVICE_DISABLED") ||
               strstr(response, "accessNotConfigured") || strstr(response, "has not been used"))) {
        snprintf(message, sizeof(message), "the %s API is not enabled in the user's Google Cloud project: the user "
                 "enables it (console.cloud.google.com, APIs & Services, Library)", service);
    } else if (status < 0) {
        snprintf(message, sizeof(message), "Google cannot be reached");
    } else {
        snprintf(message, sizeof(message), "Google answered HTTP %d", status);
    }
    if (status != 401 && status >= 0) {
        ESP_LOGW(TAG, "%s: HTTP %d: %.200s", service, status, response ? response : "");
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", message);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return text;
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
