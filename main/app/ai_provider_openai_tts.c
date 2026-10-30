/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * OpenAI speech (gpt-realtime-2.1-mini): Realtime API over a WebSocket kept open (reconnected after any error),
 * one out-of-band response per sentence:
 *
 *   session.update (voice, PCM 24 kHz, reading instructions) once connected
 *   response.create (the sentence as an input message) -> response.output_audio.delta ... -> response.done
 *
 * A session keeps its voice once it has spoken: a new voice or new instructions take a new connection.
 */

#include <stdio.h>
#include <string.h>
#include "file_costs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "app_wifi.h"
#include "tts_stream.h"
#include "tts_backend.h"

#define TTS_SAMPLE_RATE_IN      24000   /* audio/pcm: 24 kHz, 16-bit signed, mono, little-endian */
/*
 * A response that stalls (no first audio, or no more audio for a while) is given up and the connection
 * recreated: a stuck connection would otherwise block every following sentence of the reply
 */
#define TTS_FIRST_AUDIO_TIMEOUT_MS  15000
#define TTS_STALL_TIMEOUT_MS        8000
#define TTS_CONNECT_WAIT_MS     5000
#define TTS_RECONNECT_MS        2000
#define TTS_RECREATE_AFTER_MS   30000
#define TTS_WS_TASK_STACK       4608    /* internal RAM, runs the TLS handshake (~3.5 KB used) */
#define TTS_WS_BUFFER_SIZE      8192    /* PSRAM */

/* Event group bits */
#define EV_CONNECTED            BIT0
#define EV_RESPONSE_DONE        BIT1

static const char *TAG = "tts";

static EventGroupHandle_t s_events = NULL;
static esp_websocket_client_handle_t s_client = NULL;
static int64_t s_down_since = 0;
/* Settings version of the current connection (a change takes a new session) */
static uint32_t s_applied_version = 0;
/* The session configuration (voice, format, instructions) was sent on the current connection */
static bool s_session_ready = false;
/* The connection is to be recreated before the next sentence (response stalled, request not sent) */
static bool s_reconnect = false;
static char *s_uri = NULL;
static char *s_auth_header = NULL;

/* The response in progress */
static volatile bool s_response_active = false;
static volatile uint32_t s_response_generation = 0;
static int64_t s_request_us = 0;
static int64_t s_first_audio_us = 0;
static volatile int64_t s_last_audio_us = 0;
static char s_response_status[24];

/* Received frame being assembled and decoding buffer (PSRAM) */
static char *s_rx = NULL;
static size_t s_rx_len = 0;
static size_t s_rx_cap = 0;
static uint8_t *s_pcm = NULL;
static size_t s_pcm_cap = 0;

static bool grow(void **buf, size_t *cap, size_t needed)
{
    if (needed <= *cap) {
        return true;
    }
    void *p = heap_caps_realloc(*buf, needed, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        return false;
    }
    *buf = p;
    *cap = needed;
    return true;
}

static void on_audio_delta(const char *b64)
{
    if (!s_response_active || s_response_generation != tts_backend_generation()) {
        return;
    }
    size_t b64_len = strlen(b64);
    size_t pcm_len = 0;
    if (!grow((void **)&s_pcm, &s_pcm_cap, b64_len / 4 * 3 + 4) ||
            mbedtls_base64_decode(s_pcm, s_pcm_cap, &pcm_len, (const unsigned char *)b64, b64_len) != 0) {
        ESP_LOGE(TAG, "Bad audio delta");
        return;
    }
    s_last_audio_us = esp_timer_get_time();
    if (!s_first_audio_us) {
        s_first_audio_us = s_last_audio_us;
    }
    tts_backend_audio(s_pcm, pcm_len);
}

static void handle_message(const char *text)
{
    /* Audio deltas are frequent and large: find the field without parsing the whole JSON */
    if (strstr(text, "\"response.output_audio.delta\"")) {
        const char *d = strstr(text, "\"delta\"");
        d = d ? strchr(d + 7, '"') : NULL;
        if (d) {
            char *end = strchr(d + 1, '"');
            if (end) {
                *end = '\0';
                on_audio_delta(d + 1);
            }
        }
        return;
    }
    cJSON *json = cJSON_Parse(text);
    if (!json) {
        return;
    }
    const cJSON *type = cJSON_GetObjectItem(json, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";
    if (strcmp(t, "response.done") == 0) {
        // Billed tokens: text read (in, some of it cached), audio spoken (out)
        const cJSON *usage = cJSON_GetObjectItem(cJSON_GetObjectItem(json, "response"), "usage");
        const cJSON *in = cJSON_GetObjectItem(usage, "input_tokens");
        const cJSON *cached = cJSON_GetObjectItem(cJSON_GetObjectItem(usage, "input_token_details"), "cached_tokens");
        const cJSON *out = cJSON_GetObjectItem(usage, "output_tokens");
        if (cJSON_IsNumber(in) || cJSON_IsNumber(out)) {
            int cached_n = cJSON_IsNumber(cached) ? cached->valueint : 0;
            costs_add_tokens(COST_TTS, tts_backend_settings()->model, (cJSON_IsNumber(in) ? in->valueint : 0) -
                             cached_n, cached_n, 0, cJSON_IsNumber(out) ? out->valueint : 0);
        }
        const cJSON *status = cJSON_GetObjectItem(cJSON_GetObjectItem(json, "response"), "status");
        strlcpy(s_response_status, cJSON_IsString(status) ? status->valuestring : "?", sizeof(s_response_status));
        if (strcmp(s_response_status, "completed") != 0) {
            // Why: {"type": "failed", "error": {"type", "code", "message"}} (quota, rate limit, model...)
            const cJSON *details = cJSON_GetObjectItem(cJSON_GetObjectItem(json, "response"), "status_details");
            char *why = details ? cJSON_PrintUnformatted(details) : NULL;
            ESP_LOGE(TAG, "response %s: %.300s", s_response_status, why ? why : "(no details)");
            if (tts_backend_is_no_credit(why)) {
                tts_backend_no_credit();
            }
            free(why);
        }
        s_response_active = false;
        xEventGroupSetBits(s_events, EV_RESPONSE_DONE);
    } else if (strcmp(t, "error") == 0) {
        const cJSON *error = cJSON_GetObjectItem(json, "error");
        const cJSON *code = cJSON_GetObjectItem(error, "code");
        const cJSON *message = cJSON_GetObjectItem(error, "message");
        ESP_LOGE(TAG, "error: %s: %s", cJSON_IsString(code) ? code->valuestring : "-",
                 cJSON_IsString(message) ? message->valuestring : "-");
        if (tts_backend_is_no_credit(cJSON_IsString(code) ? code->valuestring : NULL) ||
                tts_backend_is_no_credit(cJSON_IsString(message) ? message->valuestring : NULL)) {
            tts_backend_no_credit();
        }
        if (s_response_active) {
            strlcpy(s_response_status, "error", sizeof(s_response_status));
            s_response_active = false;
            xEventGroupSetBits(s_events, EV_RESPONSE_DONE);
        }
    } else if (strcmp(t, "session.created") == 0 || strcmp(t, "session.updated") == 0) {
        ESP_LOGI(TAG, "%s", t);
    }
    cJSON_Delete(json);
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_rx_len = 0;
        xEventGroupSetBits(s_events, EV_CONNECTED);
        break;
    case WEBSOCKET_EVENT_DATA:
        /* Text frames; a large message arrives in several events (payload_offset / payload_len) */
        if (data->op_code != 0x01 && data->op_code != 0x00) {
            break;
        }
        if (data->payload_offset == 0) {
            s_rx_len = 0;
        }
        if (grow((void **)&s_rx, &s_rx_cap, s_rx_len + data->data_len + 1)) {
            memcpy(s_rx + s_rx_len, data->data_ptr, data->data_len);
            s_rx_len += data->data_len;
            s_rx[s_rx_len] = '\0';
        }
        if (data->payload_offset + data->data_len >= data->payload_len && s_rx_len) {
            handle_message(s_rx);
            s_rx_len = 0;
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WebSocket error (handshake status %d)", data->error_handle.esp_ws_handshake_status_code);
        xEventGroupClearBits(s_events, EV_CONNECTED);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        xEventGroupClearBits(s_events, EV_CONNECTED);
        if (s_response_active) {
            strlcpy(s_response_status, "disconnected", sizeof(s_response_status));
            s_response_active = false;
            xEventGroupSetBits(s_events, EV_RESPONSE_DONE);
        }
        break;
    default:
        break;
    }
}

static void destroy_client(void)
{
    if (s_client) {
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    xEventGroupClearBits(s_events, EV_CONNECTED);
    s_session_ready = false;
}

/* A new connection with the current settings */
static void create_client(void)
{
    destroy_client();
    s_down_since = esp_timer_get_time();
    if (WIFI_STATUS_CONNECTED_OK != wifi_connected_already()) {
        return;
    }
    // "https://api.openai.com/v1/" -> "wss://api.openai.com/v1/realtime?model=..."
    const tts_settings_t *settings = tts_backend_settings();
    s_applied_version = settings->version;
    const char *host_path = strstr(settings->base_url, "://");
    host_path = host_path ? host_path + 3 : settings->base_url;
    free(s_uri);
    free(s_auth_header);
    s_uri = NULL;
    s_auth_header = NULL;
    asprintf(&s_uri, "wss://%srealtime?model=%s", host_path, settings->model);
    asprintf(&s_auth_header, "Authorization: Bearer %s\r\n", settings->api_key);
    esp_websocket_client_config_t config = {
        .uri = s_uri,
        .headers = s_auth_header,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .task_stack = TTS_WS_TASK_STACK,
        .task_name = "tts_ws",
        .task_prio = 5,
        .buffer_size = TTS_WS_BUFFER_SIZE,
        .disable_auto_reconnect = false,
        .enable_close_reconnect = true,
        .reconnect_timeout_ms = TTS_RECONNECT_MS,
        .network_timeout_ms = 10000,
    };
    s_client = esp_websocket_client_init(&config);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to create WebSocket client");
        return;
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    if (esp_websocket_client_start(s_client) != ESP_OK) {
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
}

static bool send_json(const char *text)
{
    int len = strlen(text);
    return s_client && esp_websocket_client_send_text(s_client, text, len, pdMS_TO_TICKS(5000)) == len;
}

static bool send_cjson(cJSON *msg)
{
    char *text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    bool ok = text && send_json(text);
    free(text);
    return ok;
}

/* Audio only, verbatim reading instructions, voice and PCM 24 kHz output; no input audio */
static bool send_session_update(void)
{
    const tts_settings_t *settings = tts_backend_settings();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "session.update");
    cJSON *session = cJSON_AddObjectToObject(msg, "session");
    cJSON_AddStringToObject(session, "type", "realtime");
    cJSON_AddStringToObject(session, "model", settings->model);
    cJSON_AddItemToObject(session, "output_modalities", cJSON_CreateStringArray((const char *[]) {"audio"}, 1));
    if (settings->instructions) {
        cJSON_AddStringToObject(session, "instructions", settings->instructions);
    }
    cJSON *audio = cJSON_AddObjectToObject(session, "audio");
    cJSON_AddNullToObject(cJSON_AddObjectToObject(audio, "input"), "turn_detection");
    cJSON *output = cJSON_AddObjectToObject(audio, "output");
    cJSON *format = cJSON_AddObjectToObject(output, "format");
    cJSON_AddStringToObject(format, "type", "audio/pcm");
    cJSON_AddNumberToObject(format, "rate", TTS_SAMPLE_RATE_IN);
    cJSON_AddStringToObject(output, "voice", settings->voice ? settings->voice : "marin");
    return send_cjson(msg);
}

/*
 * One out-of-band response per sentence (conversation "none": nothing accumulates on the server).
 * The text to read is a separate input message, not part of the instructions.
 */
static bool send_response_create(const char *text)
{
    const tts_settings_t *settings = tts_backend_settings();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "response.create");
    cJSON *response = cJSON_AddObjectToObject(msg, "response");
    cJSON_AddStringToObject(response, "conversation", "none");
    cJSON_AddItemToObject(response, "output_modalities", cJSON_CreateStringArray((const char *[]) {"audio"}, 1));
    if (settings->instructions) {
        cJSON_AddStringToObject(response, "instructions", settings->instructions);
    }
    cJSON *input = cJSON_AddArrayToObject(response, "input");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "message");
    cJSON_AddStringToObject(item, "role", "user");
    cJSON *content = cJSON_AddArrayToObject(item, "content");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "type", "input_text");
    cJSON_AddStringToObject(part, "text", text);
    cJSON_AddItemToArray(content, part);
    cJSON_AddItemToArray(input, item);
    return send_cjson(msg);
}

static void openai_start(void)
{
    if (!s_events) {
        s_events = xEventGroupCreate();
    }
    create_client();
}

static void openai_stop(void)
{
    destroy_client();
    ESP_LOGI(TAG, "OpenAI speech closed");
}

/* Connection supervision: (re)send the session configuration after each connection; new settings */
static void openai_upkeep(void)
{
    if (s_applied_version != tts_backend_settings()->version || s_reconnect) {
        s_reconnect = false;
        ESP_LOGI(TAG, "voice: %s, reconnecting", tts_backend_settings()->voice);
        create_client();
    }
    bool connected = s_client && esp_websocket_client_is_connected(s_client) &&
                     (xEventGroupGetBits(s_events) & EV_CONNECTED);
    if (connected && !s_session_ready) {
        if (send_session_update()) {
            s_session_ready = true;
            ESP_LOGI(TAG, "connected in %lld ms", (esp_timer_get_time() - s_down_since) / 1000);
        }
    } else if (!connected && s_session_ready) {
        s_session_ready = false;
        s_down_since = esp_timer_get_time();
        ESP_LOGW(TAG, "connection lost, reconnecting");
    }
    if (!connected && esp_timer_get_time() - s_down_since > TTS_RECREATE_AFTER_MS * 1000LL) {
        ESP_LOGW(TAG, "still disconnected, recreating the WebSocket client");
        create_client();
    }
}

static void openai_say(const char *text, uint32_t generation)
{
    /* A voice chosen just before this sentence (e.g. its sample) must already apply to it */
    if (s_applied_version != tts_backend_settings()->version) {
        openai_upkeep();
    }
    tts_backend_sentence_start(generation);
    if (!s_client || !(xEventGroupWaitBits(s_events, EV_CONNECTED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(TTS_CONNECT_WAIT_MS)) & EV_CONNECTED)) {
        ESP_LOGE(TAG, "Not connected: sentence dropped");
        tts_backend_sentence_end("not connected", true);
        return;
    }
    /* Just (re)connected: the session configuration must go first */
    if (!s_session_ready && send_session_update()) {
        s_session_ready = true;
    }
    s_first_audio_us = 0;
    s_response_generation = generation;
    s_response_active = true;
    xEventGroupClearBits(s_events, EV_RESPONSE_DONE);
    s_request_us = esp_timer_get_time();
    if (!send_response_create(text)) {
        s_response_active = false;
        ESP_LOGE(TAG, "Request not sent");
        s_reconnect = true;
        tts_backend_sentence_end("not sent", true);
        return;
    }

    /* Wait for the end of the response; cancel it if the reply is cancelled */
    bool cancel_sent = false;
    bool stalled = false;
    while (!(xEventGroupWaitBits(s_events, EV_RESPONSE_DONE, pdFALSE, pdFALSE, pdMS_TO_TICKS(50)) & EV_RESPONSE_DONE)) {
        if (generation != tts_backend_generation() && !cancel_sent) {
            send_json("{\"type\":\"response.cancel\"}");
            cancel_sent = true;
        }
        int64_t now = esp_timer_get_time();
        if (s_first_audio_us ? now - s_last_audio_us > TTS_STALL_TIMEOUT_MS * 1000LL :
                now - s_request_us > TTS_FIRST_AUDIO_TIMEOUT_MS * 1000LL) {
            ESP_LOGE(TAG, "Response stalled (%s), reconnecting", s_first_audio_us ? "no more audio" : "no audio");
            s_response_active = false;
            s_reconnect = true;
            stalled = true;
            break;
        }
    }
    tts_backend_sentence_end(stalled ? "stalled" : s_response_status, stalled);
}

const tts_backend_t tts_backend_openai = {
    .name = "openai",
    .start = openai_start,
    .stop = openai_stop,
    .upkeep = openai_upkeep,
    .say = openai_say,
};
