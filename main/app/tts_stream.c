/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Sentence-by-sentence speech synthesis with the OpenAI Realtime API (gpt-realtime-2.1-mini).
 *
 *   tts_stream_say() -> sentence queue -> [tts task] response.create (one out-of-band response per
 *   sentence) -> response.output_audio.delta, 24 kHz -> 16 kHz -> audio stream buffer (PSRAM)
 *   -> [playback task] I2S (16 kHz stereo)
 *
 * The WebSocket stays open (reconnected after any error). The I2S bus stays at 16 kHz, shared with
 * the microphones, so that listening is never interrupted.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "bsp_board.h"
#include "app_wifi.h"
#include "tts_stream.h"

#define TTS_SAMPLE_RATE_IN      24000   /* audio/pcm: 24 kHz, 16-bit signed, mono, little-endian */
#define TTS_AUDIO_BUFFER_BYTES  (256 * 1024)    /* 8 s of 16 kHz mono audio, in PSRAM */
#define TTS_SENTENCE_QUEUE_LEN  16
/*
 * A response that stalls (no first audio, or no more audio for a while) is given up and the connection
 * recreated: a stuck connection would otherwise block every following sentence of the reply
 */
#define TTS_FIRST_AUDIO_TIMEOUT_MS  15000
#define TTS_STALL_TIMEOUT_MS        8000
/* Cuts in the middle of a sentence from which the reply is reported as choppy */
#define TTS_CHOPPY_CUTS             2
#define TTS_CONNECT_WAIT_MS     5000
#define TTS_RECONNECT_MS        2000
#define TTS_RECREATE_AFTER_MS   30000
#define TTS_WS_TASK_STACK       6144    /* internal RAM, runs the TLS handshake */
#define TTS_WS_BUFFER_SIZE      8192    /* PSRAM */
#define PLAYBACK_FRAMES         256

#define RESAMPLER_TAPS          15

/* Event group bits */
#define EV_CONNECTED            BIT0
#define EV_RESPONSE_DONE        BIT1

static const char *TAG = "tts";

typedef struct {
    char *text;             /* NULL marks the end of the reply */
    uint32_t generation;
} sentence_item_t;

static QueueHandle_t s_sentences = NULL;
static StreamBufferHandle_t s_audio = NULL;
static EventGroupHandle_t s_events = NULL;

/* Incremented for each new reply and on cancel: work tagged with an older value is dropped */
static volatile uint32_t s_generation = 0;
/* Generation whose last sentence has been synthesized */
static volatile uint32_t s_synth_done_generation = 0;
/* Set on cancel: the playback task drops the buffered audio */
static volatile bool s_flush = false;
static tts_event_cb_t s_event_cb = NULL;

static char *s_uri = NULL;
static char *s_auth_header = NULL;
static char *s_model = NULL;
static char *s_voice = NULL;
/* Voice requested by tts_stream_set_voice(), applied by the synthesis task (empty: none) */
static char s_pending_voice[24];
static char *s_pending_instructions = NULL;
static volatile bool s_voice_change = false;    /* voice and/or instructions changed: new session */
/* Last audio sent to the speaker (tts_stream_is_silent) */
static volatile int64_t s_last_play_us = 0;
/* The session configuration (voice, format, instructions) was sent on the current connection */
static bool s_session_ready = false;
static char *s_instructions = NULL;

/* ---------- 24 kHz -> 16 kHz resampler ---------- */

/* Low-pass FIR at 7 kHz (below the 8 kHz Nyquist of the output), Q15 */
static int16_t s_fir[RESAMPLER_TAPS];

typedef struct {
    int16_t history[RESAMPLER_TAPS];
    int pos;            /* index of the next input sample, modulo 3 */
    int32_t pending;    /* filtered sample kept for the averaged output */
    bool has_odd_byte;
    uint8_t odd_byte;
} resampler_t;

static void resampler_design(void)
{
    const float fc = 7000.0f / TTS_SAMPLE_RATE_IN;
    const int mid = RESAMPLER_TAPS / 2;
    float coef[RESAMPLER_TAPS];
    float sum = 0;
    for (int i = 0; i < RESAMPLER_TAPS; i++) {
        int n = i - mid;
        float sinc = (n == 0) ? 2.0f * fc : sinf(2.0f * (float)M_PI * fc * n) / ((float)M_PI * n);
        float window = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * i / (RESAMPLER_TAPS - 1));
        coef[i] = sinc * window;
        sum += coef[i];
    }
    for (int i = 0; i < RESAMPLER_TAPS; i++) {
        s_fir[i] = (int16_t)lrintf(coef[i] / sum * 32767.0f);
    }
}

static int16_t clamp16(int32_t v)
{
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v);
}

/*
 * Filter and decimate by 3/2: of every 3 filtered input samples y0 y1 y2, output y0 and (y1 + y2) / 2
 * (the sample half-way between them). Returns the number of output samples written to out.
 */
static size_t resample(resampler_t *r, const uint8_t *bytes, size_t len, int16_t *out)
{
    size_t n_out = 0;
    size_t i = 0;
    while (i < len) {
        int16_t x;
        if (r->has_odd_byte) {
            x = (int16_t)(r->odd_byte | (bytes[i] << 8));
            r->has_odd_byte = false;
            i += 1;
        } else if (i + 1 < len) {
            x = (int16_t)(bytes[i] | (bytes[i + 1] << 8));
            i += 2;
        } else {
            r->odd_byte = bytes[i];
            r->has_odd_byte = true;
            break;
        }
        memmove(r->history, r->history + 1, (RESAMPLER_TAPS - 1) * sizeof(int16_t));
        r->history[RESAMPLER_TAPS - 1] = x;
        int32_t acc = 0;
        for (int k = 0; k < RESAMPLER_TAPS; k++) {
            acc += (int32_t)s_fir[k] * r->history[k];
        }
        int32_t y = acc >> 15;
        switch (r->pos) {
        case 0:
            out[n_out++] = clamp16(y);
            break;
        case 1:
            r->pending = y;
            break;
        default:
            out[n_out++] = clamp16((r->pending + y) / 2);
            break;
        }
        r->pos = (r->pos + 1) % 3;
    }
    return n_out;
}

/* ---------- synthesis (Realtime WebSocket) ---------- */

/* Audio of the sentence being spoken, filled by the WebSocket task */
static resampler_t s_resampler;
static volatile uint32_t s_response_generation = 0;    /* generation of the response in progress */
static volatile bool s_response_active = false;
static int64_t s_request_us = 0;
static int64_t s_first_audio_us = 0;
static volatile int64_t s_last_audio_us = 0;
static size_t s_audio_bytes = 0;
/* Quality of the current reply: speech run dry in the middle of a sentence, sentences lost */
static volatile unsigned s_cuts = 0;
static volatile bool s_failed = false;
/* The connection is to be recreated before the next sentence (response stalled, request not sent) */
static bool s_reconnect = false;
static char s_response_status[24];

/* Received frame being assembled and decoding buffers (PSRAM) */
static char *s_rx = NULL;
static size_t s_rx_len = 0;
static size_t s_rx_cap = 0;
static uint8_t *s_pcm = NULL;
static size_t s_pcm_cap = 0;
static int16_t *s_resampled = NULL;
static size_t s_resampled_cap = 0;

static void replace_str(char **dst, const char *src)
{
    free(*dst);
    *dst = (src && src[0]) ? strdup(src) : NULL;
}

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

void tts_stream_configure(const char *base_url, const char *api_key, const char *model, const char *voice,
                          const char *instructions)
{
    /* "https://api.openai.com/v1/" -> "wss://api.openai.com/v1/realtime?model=..." */
    const char *host_path = strstr(base_url, "://");
    host_path = host_path ? host_path + 3 : base_url;
    char *uri = NULL;
    char *auth = NULL;
    asprintf(&uri, "wss://%srealtime?model=%s", host_path, model);
    asprintf(&auth, "Authorization: Bearer %s\r\n", api_key);
    free(s_uri);
    free(s_auth_header);
    s_uri = uri;
    s_auth_header = auth;
    replace_str(&s_model, model);
    replace_str(&s_voice, voice);
    replace_str(&s_instructions, instructions);
    ESP_LOGI(TAG, "model: %s, voice: %s", s_model ? s_model : "-", s_voice ? s_voice : "-");
}

void tts_stream_set_voice(const char *voice)
{
    strlcpy(s_pending_voice, voice, sizeof(s_pending_voice));
    s_voice_change = true;
}

void tts_stream_set_instructions(const char *instructions)
{
    char *copy = strdup(instructions ? instructions : "");
    char *old = s_pending_instructions;
    s_pending_instructions = copy;
    free(old);
    s_voice_change = true;
}

/* Send audio to the playback buffer, giving up if the reply was cancelled meanwhile */
static bool push_audio(const int16_t *samples, size_t count, uint32_t generation)
{
    const uint8_t *p = (const uint8_t *)samples;
    size_t left = count * sizeof(int16_t);
    while (left > 0) {
        if (generation != s_generation) {
            return false;
        }
        size_t sent = xStreamBufferSend(s_audio, p, left, pdMS_TO_TICKS(100));
        p += sent;
        left -= sent;
    }
    return true;
}

static void on_audio_delta(const char *b64)
{
    uint32_t generation = s_response_generation;
    if (!s_response_active || generation != s_generation) {
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
    s_audio_bytes += pcm_len;
    if (!grow((void **)&s_resampled, &s_resampled_cap, (pcm_len / 3 + 2) * sizeof(int16_t))) {
        return;
    }
    size_t count = resample(&s_resampler, s_pcm, pcm_len, s_resampled);
    push_audio(s_resampled, count, generation);
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
        const cJSON *status = cJSON_GetObjectItem(cJSON_GetObjectItem(json, "response"), "status");
        strlcpy(s_response_status, cJSON_IsString(status) ? status->valuestring : "?", sizeof(s_response_status));
        s_response_active = false;
        xEventGroupSetBits(s_events, EV_RESPONSE_DONE);
    } else if (strcmp(t, "error") == 0) {
        const cJSON *error = cJSON_GetObjectItem(json, "error");
        const cJSON *code = cJSON_GetObjectItem(error, "code");
        const cJSON *message = cJSON_GetObjectItem(error, "message");
        ESP_LOGE(TAG, "error: %s: %s", cJSON_IsString(code) ? code->valuestring : "-",
                 cJSON_IsString(message) ? message->valuestring : "-");
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

static esp_websocket_client_handle_t create_client(void)
{
    esp_websocket_client_config_t config = {
        .uri = s_uri,
        .headers = s_auth_header,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .task_stack = TTS_WS_TASK_STACK,
        .task_prio = 5,
        .buffer_size = TTS_WS_BUFFER_SIZE,
        .disable_auto_reconnect = false,
        .enable_close_reconnect = true,
        .reconnect_timeout_ms = TTS_RECONNECT_MS,
        .network_timeout_ms = 10000,
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to create WebSocket client");
        return NULL;
    }
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    if (esp_websocket_client_start(client) != ESP_OK) {
        esp_websocket_client_destroy(client);
        return NULL;
    }
    return client;
}

static bool send_json(esp_websocket_client_handle_t client, const char *text)
{
    int len = strlen(text);
    return esp_websocket_client_send_text(client, text, len, pdMS_TO_TICKS(5000)) == len;
}

static bool send_cjson(esp_websocket_client_handle_t client, cJSON *msg)
{
    char *text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    bool ok = text && send_json(client, text);
    free(text);
    return ok;
}

/* Audio only, verbatim reading instructions, voice and PCM 24 kHz output; no input audio */
static bool send_session_update(esp_websocket_client_handle_t client)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "session.update");
    cJSON *session = cJSON_AddObjectToObject(msg, "session");
    cJSON_AddStringToObject(session, "type", "realtime");
    cJSON_AddStringToObject(session, "model", s_model);
    cJSON_AddItemToObject(session, "output_modalities", cJSON_CreateStringArray((const char *[]) {"audio"}, 1));
    if (s_instructions) {
        cJSON_AddStringToObject(session, "instructions", s_instructions);
    }
    cJSON *audio = cJSON_AddObjectToObject(session, "audio");
    cJSON_AddNullToObject(cJSON_AddObjectToObject(audio, "input"), "turn_detection");
    cJSON *output = cJSON_AddObjectToObject(audio, "output");
    cJSON *format = cJSON_AddObjectToObject(output, "format");
    cJSON_AddStringToObject(format, "type", "audio/pcm");
    cJSON_AddNumberToObject(format, "rate", TTS_SAMPLE_RATE_IN);
    cJSON_AddStringToObject(output, "voice", s_voice);
    return send_cjson(client, msg);
}

/*
 * One out-of-band response per sentence (conversation "none": nothing accumulates on the server).
 * The text to read is a separate input message, not part of the instructions.
 */
static bool send_response_create(esp_websocket_client_handle_t client, const char *text)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "response.create");
    cJSON *response = cJSON_AddObjectToObject(msg, "response");
    cJSON_AddStringToObject(response, "conversation", "none");
    cJSON_AddItemToObject(response, "output_modalities", cJSON_CreateStringArray((const char *[]) {"audio"}, 1));
    if (s_instructions) {
        cJSON_AddStringToObject(response, "instructions", s_instructions);
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
    return send_cjson(client, msg);
}

static void synthesize(esp_websocket_client_handle_t client, const char *text, uint32_t generation)
{
    if (!(xEventGroupWaitBits(s_events, EV_CONNECTED, pdFALSE, pdFALSE, pdMS_TO_TICKS(TTS_CONNECT_WAIT_MS)) &
            EV_CONNECTED)) {
        ESP_LOGE(TAG, "Not connected: sentence dropped");
        s_failed = true;
        return;
    }
    /* Just (re)connected: the session configuration must go first */
    if (!s_session_ready && send_session_update(client)) {
        s_session_ready = true;
    }
    s_resampler = (resampler_t) {0};
    s_first_audio_us = 0;
    s_audio_bytes = 0;
    s_response_generation = generation;
    s_response_active = true;
    xEventGroupClearBits(s_events, EV_RESPONSE_DONE);
    s_request_us = esp_timer_get_time();
    if (!send_response_create(client, text)) {
        s_response_active = false;
        ESP_LOGE(TAG, "Request not sent");
        s_failed = true;
        s_reconnect = true;
        return;
    }

    /* Wait for the end of the response; cancel it if the reply is cancelled */
    bool cancel_sent = false;
    while (!(xEventGroupWaitBits(s_events, EV_RESPONSE_DONE, pdFALSE, pdFALSE, pdMS_TO_TICKS(50)) & EV_RESPONSE_DONE)) {
        if (generation != s_generation && !cancel_sent) {
            send_json(client, "{\"type\":\"response.cancel\"}");
            cancel_sent = true;
        }
        int64_t now = esp_timer_get_time();
        if (s_first_audio_us ? now - s_last_audio_us > TTS_STALL_TIMEOUT_MS * 1000LL :
                now - s_request_us > TTS_FIRST_AUDIO_TIMEOUT_MS * 1000LL) {
            ESP_LOGE(TAG, "Response stalled (%s), reconnecting", s_first_audio_us ? "no more audio" : "no audio");
            s_response_active = false;
            s_failed = true;
            s_reconnect = true;
            break;
        }
    }
    ESP_LOGI(TAG, "\"%.40s%s\": first audio %lld ms, total %lld ms, %u bytes, %s", text, strlen(text) > 40 ? "..." : "",
             s_first_audio_us ? (s_first_audio_us - s_request_us) / 1000 : -1LL,
             (esp_timer_get_time() - s_request_us) / 1000, (unsigned)s_audio_bytes, s_response_status);
}

/* A new connection, after a stalled response or a request that could not be sent */
static void reconnect(esp_websocket_client_handle_t *client, int64_t *down_since)
{
    s_reconnect = false;
    if (*client) {
        esp_websocket_client_stop(*client);
        esp_websocket_client_destroy(*client);
    }
    xEventGroupClearBits(s_events, EV_CONNECTED);
    s_session_ready = false;
    *client = (WIFI_STATUS_CONNECTED_OK == wifi_connected_already()) ? create_client() : NULL;
    *down_since = esp_timer_get_time();
}

/* New voice: a new session (a session cannot change its voice once it has spoken) */
static void apply_voice_change(esp_websocket_client_handle_t *client, int64_t *down_since)
{
    if (!s_voice_change) {
        return;
    }
    s_voice_change = false;
    if (s_pending_voice[0]) {
        replace_str(&s_voice, s_pending_voice);
        s_pending_voice[0] = '\0';
    }
    char *instructions = s_pending_instructions;
    s_pending_instructions = NULL;
    if (instructions) {
        replace_str(&s_instructions, instructions);
        free(instructions);
    }
    ESP_LOGI(TAG, "voice: %s, reconnecting", s_voice);
    if (*client) {
        esp_websocket_client_stop(*client);
        esp_websocket_client_destroy(*client);
    }
    xEventGroupClearBits(s_events, EV_CONNECTED);
    s_session_ready = false;
    *client = (WIFI_STATUS_CONNECTED_OK == wifi_connected_already()) ? create_client() : NULL;
    *down_since = esp_timer_get_time();
}

static void tts_task(void *arg)
{
    /* Nothing to do before the network is up */
    while (WIFI_STATUS_CONNECTED_OK != wifi_connected_already()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    esp_websocket_client_handle_t client = create_client();
    int64_t down_since = esp_timer_get_time();

    for (;;) {
        apply_voice_change(&client, &down_since);

        /* Connection supervision: (re)send the session configuration after each connection */
        bool connected = client && esp_websocket_client_is_connected(client) &&
                         (xEventGroupGetBits(s_events) & EV_CONNECTED);
        if (connected && !s_session_ready) {
            if (send_session_update(client)) {
                s_session_ready = true;
                ESP_LOGI(TAG, "connected in %lld ms", (esp_timer_get_time() - down_since) / 1000);
            }
        } else if (!connected && s_session_ready) {
            s_session_ready = false;
            down_since = esp_timer_get_time();
            ESP_LOGW(TAG, "connection lost, reconnecting");
        }
        if (!connected && esp_timer_get_time() - down_since > TTS_RECREATE_AFTER_MS * 1000LL) {
            ESP_LOGW(TAG, "still disconnected, recreating the WebSocket client");
            if (client) {
                esp_websocket_client_stop(client);
                esp_websocket_client_destroy(client);
                xEventGroupClearBits(s_events, EV_CONNECTED);
            }
            client = (WIFI_STATUS_CONNECTED_OK == wifi_connected_already()) ? create_client() : NULL;
            down_since = esp_timer_get_time();
        }

        sentence_item_t item;
        if (xQueueReceive(s_sentences, &item, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        /* A voice chosen just before this sentence (e.g. its sample) must already apply to it */
        apply_voice_change(&client, &down_since);
        if (item.generation == s_generation) {
            if (item.text && client) {
                synthesize(client, item.text, item.generation);
                if (s_reconnect) {
                    reconnect(&client, &down_since);
                }
            } else if (item.text) {
                s_failed = true;    // no connection: the sentence is lost
            } else {
                s_synth_done_generation = item.generation;
            }
        }
        free(item.text);
    }
}

/* ---------- playback ---------- */

/*
 * Audio kept in reserve before playing: the first bytes of a sentence arrive in small irregular bursts,
 * and playing them as they come starves the I2S output (crackles). Once a sentence is flowing, the
 * synthesis runs faster than real time.
 */
#define PREBUFFER_START_BYTES   (200 * 16 * sizeof(int16_t))    /* 200 ms at 16 kHz mono */
#define PREBUFFER_RESUME_BYTES  (100 * 16 * sizeof(int16_t))    /* 100 ms after running dry */
/*
 * When the synthesis is slower than real time (slow network or service), each underrun doubles the reserve
 * waited for before resuming (100 ms, 200 ms ... 1.6 s): a few longer pauses instead of choppy speech
 */
#define PREBUFFER_RESUME_MAX_SHIFT  4

static void playback_task(void *arg)
{
    static int16_t mono[PLAYBACK_FRAMES];
    static int16_t stereo[PLAYBACK_FRAMES * 2];
    uint32_t playing_generation = 0;
    bool started = false;       /* first audio of the reply played */
    bool playing = false;       /* feeding the I2S output (else filling the reserve) */
    bool done_sent = true;
    unsigned underruns = 0;

    for (;;) {
        if (s_flush) {
            while (xStreamBufferReceive(s_audio, mono, sizeof(mono), 0) > 0) {
            }
            s_flush = false;
        }
        uint32_t generation = s_generation;
        if (generation != playing_generation) {
            playing_generation = generation;
            started = false;
            playing = false;
            done_sent = false;
            underruns = 0;
        }
        bool synth_done = (s_synth_done_generation == playing_generation);
        size_t available = xStreamBufferBytesAvailable(s_audio);

        if (!playing) {
            unsigned shift = underruns < PREBUFFER_RESUME_MAX_SHIFT ? underruns : PREBUFFER_RESUME_MAX_SHIFT;
            size_t needed = started ? (PREBUFFER_RESUME_BYTES << shift) : PREBUFFER_START_BYTES;
            if (available >= needed || (synth_done && available > 0)) {
                playing = true;
            } else {
                if (synth_done && available == 0 && !done_sent) {
                    done_sent = true;
                    ESP_LOGI(TAG, "reply played, %u underrun(s), %u cut(s) mid-sentence%s", underruns,
                             (unsigned)s_cuts, s_failed ? ", sentence(s) lost" : "");
                    if (s_event_cb) {
                        s_event_cb(TTS_EVENT_DONE);
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
        }

        size_t n = xStreamBufferReceive(s_audio, mono, sizeof(mono), pdMS_TO_TICKS(20));
        if (n >= sizeof(int16_t)) {
            if (!started) {
                started = true;
                if (s_event_cb) {
                    s_event_cb(TTS_EVENT_FIRST_AUDIO);
                }
            }
            size_t frames = n / sizeof(int16_t);
            for (size_t i = 0; i < frames; i++) {
                stereo[2 * i] = mono[i];
                stereo[2 * i + 1] = mono[i];
            }
            size_t written = 0;
            bsp_i2s_write(stereo, frames * 2 * sizeof(int16_t), &written, portMAX_DELAY);
            s_last_play_us = esp_timer_get_time();
        } else {
            /* Ran dry: wait for the end of the reply, or refill the reserve before resuming */
            playing = false;
            if (!synth_done) {
                underruns++;
                /* Not a pause between two sentences: speech cut while a sentence was arriving */
                if (s_response_active && s_first_audio_us && s_response_generation == playing_generation) {
                    s_cuts++;
                }
            }
        }
    }
}

/* ---------- public API ---------- */

bool tts_stream_reply_was_choppy(void)
{
    return s_failed || s_cuts >= TTS_CHOPPY_CUTS;
}

bool tts_stream_is_silent(uint32_t ms)
{
    return esp_timer_get_time() - s_last_play_us >= (int64_t)ms * 1000;
}

esp_err_t tts_stream_init(void)
{
    ESP_RETURN_ON_FALSE(!s_sentences, ESP_ERR_INVALID_STATE, TAG, "Already initialized");
    resampler_design();
    s_sentences = xQueueCreate(TTS_SENTENCE_QUEUE_LEN, sizeof(sentence_item_t));
    s_audio = xStreamBufferCreateWithCaps(TTS_AUDIO_BUFFER_BYTES, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_sentences && s_audio && s_events, ESP_ERR_NO_MEM, TAG, "Failed to create queues");

    /* Stacks in PSRAM: internal RAM is too fragmented for 8 KB blocks once Wi-Fi and the AFE run */
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(tts_task, "tts", 8 * 1024, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
    ok &= xTaskCreatePinnedToCoreWithCaps(playback_task, "tts_play", 4 * 1024, NULL, 6, NULL, 1, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "Failed to create tasks");
    return ESP_OK;
}

static void drop_queued_sentences(void)
{
    sentence_item_t item;
    while (xQueueReceive(s_sentences, &item, 0) == pdTRUE) {
        free(item.text);
    }
}

void tts_stream_begin(tts_event_cb_t cb)
{
    s_generation++;
    s_cuts = 0;
    s_failed = false;
    drop_queued_sentences();
    s_flush = true;
    s_event_cb = cb;
}

void tts_stream_say(const char *sentence)
{
    size_t len = strlen(sentence) + 1;
    sentence_item_t item = {
        .text = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .generation = s_generation,
    };
    if (item.text) {
        memcpy(item.text, sentence, len);
    }
    if (item.text && xQueueSend(s_sentences, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Sentence queue full, dropped: %.40s", sentence);
        free(item.text);
    }
}

void tts_stream_end(void)
{
    sentence_item_t item = {
        .text = NULL,
        .generation = s_generation,
    };
    xQueueSend(s_sentences, &item, portMAX_DELAY);
}

void tts_stream_cancel(void)
{
    s_generation++;
    drop_queued_sentences();
    s_flush = true;
}
