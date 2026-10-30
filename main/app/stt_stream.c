/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Streaming speech-to-text over a Realtime WebSocket, the protocol of the service chosen ([stt] of AGENTS.INI)
 * given by its backend (ai_provider_openai_stt.c, ai_provider_mistral_stt.c, ai_provider_deepgram_stt.c).
 *
 * Each listening session is a "turn":
 *
 *   detect task (AFE output, 16 kHz) -> stt_stream_write() -> audio buffer (PSRAM)
 *   -> [stt task] (16 kHz -> 24 kHz for OpenAI), base64 -> append messages (Deepgram: binary frames)
 *   end of speech (local VAD) -> commit message -> ... final transcript
 *
 * OpenAI's WebSocket stays open (connected at boot, reconnected after any error or server close), so that
 * listening never waits for a TLS handshake; Mistral's and Deepgram's sessions last a question: opened when
 * listening starts
 * (the audio is buffered meanwhile) and closed once transcribed. The end of speech is detected on the device.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "file_costs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
#include "app_wifi.h"
#include "stt_stream.h"
#include "stt_backend.h"

#define STT_INPUT_RATE          16000
#define STT_AUDIO_BUFFER_BYTES  (10 * STT_INPUT_RATE * 2)   /* 10 s of audio while (re)connecting */
/*
 * Audio per append message: at least 100 ms in steady state (each message costs a WebSocket frame and
 * TLS records), up to 200 ms to catch up with buffered audio (larger bursts delay the other connections
 * sharing the Wi-Fi).
 */
#define STT_MIN_CHUNK_SAMPLES   1600
#define STT_MAX_CHUNK_SAMPLES   3200
#define STT_CHUNK_OUT_SAMPLES   (STT_MAX_CHUNK_SAMPLES * 3 / 2 + 2)
#define STT_RESULT_TIMEOUT_MS   10000
/* Automatic reconnection by the WebSocket client; recreated from scratch if still down after this */
#define STT_RECONNECT_MS        2000
#define STT_RECREATE_AFTER_MS   30000
#define STT_WS_TASK_STACK       4608    /* internal RAM, runs the TLS handshake (~3.6 KB used) */
/* Audio heard billed by duration: counted as this many tokens a second (OpenAI's rate for audio input) */
#define STT_TOKENS_PER_SECOND   10
#define STT_WS_BUFFER_SIZE      8192    /* PSRAM; larger frames, fewer TLS records */
#define STT_POLL_MS             50

/* Event group bits */
#define EV_TURN_START       BIT0    /* command: a listening session starts */
#define EV_COMMIT           BIT1    /* command: end of speech */
#define EV_ABORT            BIT2    /* command: drop the turn */
#define EV_CONNECTED        BIT3    /* WebSocket connected (set / cleared by the event handler) */
#define EV_TRANSCRIPT       BIT4    /* final transcript of the turn received */
#define EV_FAILED           BIT5    /* the turn failed */

static const char *TAG = "stt";

static EventGroupHandle_t s_events = NULL;
static StreamBufferHandle_t s_audio = NULL;
static volatile bool s_capturing = false;   /* stt_stream_write() accepts audio */
static volatile bool s_turn_active = false; /* between stt_stream_start() and the end of the turn */
static volatile bool s_awaiting = false;    /* commit sent, waiting for the final transcript */
static bool s_overflow_logged = false;
static stt_partial_cb_t s_on_partial = NULL;

static const stt_backend_t *s_backend = NULL;
static char *s_uri = NULL;
static char *s_auth_header = NULL;
static char *s_model = NULL;
static char *s_language = NULL;
/* Service, model or key changed by stt_stream_configure(): a new connection (applied by the stt task) */
static volatile bool s_reconfigure = false;
/* Audio sent in the current turn (16 kHz samples), for a service billed by duration */
static size_t s_turn_sent_samples = 0;
/* Time from the commit to the final transcript of the last turn (model test) */
static int s_last_final_ms = -1;
/* Language requested by stt_stream_set_language(), applied by the connection task */
static char s_pending_language[8];
static volatile bool s_language_change = false;

/* Received frame being assembled, partial and final transcripts (PSRAM) */
static char *s_rx = NULL;
static size_t s_rx_len = 0;
static size_t s_rx_cap = 0;
static char *s_partial = NULL;
static size_t s_partial_len = 0;
static size_t s_partial_cap = 0;
static char *s_transcript = NULL;
static int64_t s_commit_us = 0;

/* Send buffers (PSRAM) */
static int16_t *s_in = NULL;
static int16_t *s_out = NULL;
static char *s_message = NULL;
static size_t s_message_cap = 0;

/* ---------- helpers ---------- */

static bool append(char **buf, size_t *len, size_t *cap, const char *data, size_t n)
{
    if (*len + n + 1 > *cap) {
        size_t new_cap = *cap ? *cap : 512;
        while (new_cap < *len + n + 1) {
            new_cap *= 2;
        }
        char *p = heap_caps_realloc(*buf, new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) {
            return false;
        }
        *buf = p;
        *cap = new_cap;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    (*buf)[*len] = '\0';
    return true;
}

static void replace_str(char **dst, const char *src)
{
    free(*dst);
    *dst = (src && src[0]) ? strdup(src) : NULL;
}

/* 16 kHz -> 24 kHz by linear interpolation: 3 output samples for every 2 input samples */
typedef struct {
    int16_t prev;
    int phase;      /* position of the next output between prev and the next input, in thirds */
} upsampler_t;

static upsampler_t s_upsampler;

static size_t upsample(upsampler_t *u, const int16_t *in, size_t n, int16_t *out)
{
    size_t n_out = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t x = in[i];
        while (u->phase < 3) {
            out[n_out++] = (int16_t)(u->prev + (x - u->prev) * u->phase / 3);
            u->phase += 2;
        }
        u->phase -= 3;
        u->prev = (int16_t)x;
    }
    return n_out;
}

/* ---------- received events (WebSocket task) ---------- */

/* The last transcription refused for lack of credit */
static volatile bool s_no_credit = false;

void stt_backend_delta(const char *text)
{
    /* Deltas of a turn that is over (late events) are ignored */
    if (s_turn_active && append(&s_partial, &s_partial_len, &s_partial_cap, text, strlen(text)) && s_on_partial) {
        s_on_partial(s_partial);
    }
}

void stt_backend_final(const char *text, int in_tokens, int out_tokens, double seconds)
{
    // Billed: tokens of audio heard (in) and of text written (out), or the duration of the audio heard, counted
    // as STT_TOKENS_PER_SECOND tokens (so that a price per million tokens applies)
    if (in_tokens || out_tokens) {
        costs_add_tokens(COST_STT, s_model, in_tokens, 0, 0, out_tokens);
    } else {
        if (seconds < 0) {
            seconds = (double)s_turn_sent_samples / STT_INPUT_RATE;
        }
        costs_add_tokens(COST_STT, s_model, (int)(seconds * STT_TOKENS_PER_SECOND), 0, 0, 0);
    }
    if (!s_awaiting) {
        return;
    }
    free(s_transcript);
    s_transcript = strdup(text ? text : (s_partial_len ? s_partial : ""));
    s_awaiting = false;
    s_turn_active = false;
    s_last_final_ms = (int)((esp_timer_get_time() - s_commit_us) / 1000);
    ESP_LOGI(TAG, "final transcript %lld ms after commit: \"%s\"", (esp_timer_get_time() - s_commit_us) / 1000,
             s_transcript ? s_transcript : "");
    s_no_credit = false;
    xEventGroupSetBits(s_events, EV_TRANSCRIPT);
}

void stt_backend_failed(const char *code, const char *message)
{
    ESP_LOGE(TAG, "%s error: %s: %s", s_backend ? s_backend->name : "?", code && code[0] ? code : "-",
             message ? message : "-");
    // The credit exhausted: "insufficient_quota", "You exceeded your current quota...", 402...
    const char *c = code ? code : "";
    const char *m = message ? message : "";
    s_no_credit = strstr(c, "quota") || strcmp(c, "402") == 0 || strcasestr(m, "quota") ||
                  strcasestr(m, "billing") || strcasestr(m, "credit");
    if (s_awaiting || s_turn_active) {
        s_awaiting = false;
        s_turn_active = false;
        s_capturing = false;
        xEventGroupSetBits(s_events, EV_FAILED);
    }
}

static void handle_message(const char *text)
{
    cJSON *json = cJSON_Parse(text);
    if (!json) {
        ESP_LOGW(TAG, "Unparsable message: %.100s", text);
        return;
    }
    const cJSON *type = cJSON_GetObjectItem(json, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";
    if (strcmp(t, "session.created") == 0 || strcmp(t, "session.updated") == 0) {
        ESP_LOGI(TAG, "%s", t);
    } else if (s_backend) {
        s_backend->on_message(json, t);
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
        append(&s_rx, &s_rx_len, &s_rx_cap, data->data_ptr, data->data_len);
        if (data->payload_offset + data->data_len >= data->payload_len && s_rx_len) {
            handle_message(s_rx);
            s_rx_len = 0;
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WebSocket error (handshake status %d, tls 0x%x)",
                 data->error_handle.esp_ws_handshake_status_code, data->error_handle.esp_tls_last_esp_err);
        xEventGroupClearBits(s_events, EV_CONNECTED);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        xEventGroupClearBits(s_events, EV_CONNECTED);
        break;
    default:
        break;
    }
}

/* ---------- connection (stt task) ---------- */

static esp_websocket_client_handle_t create_client(void)
{
    // Its URI made now: it may carry the language in use (Deepgram)
    free(s_uri);
    s_uri = s_backend->uri(s_model, s_language);
    if (!s_uri || !s_auth_header) {
        return NULL;
    }
    esp_websocket_client_config_t config = {
        .uri = s_uri,
        .headers = s_auth_header,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .task_stack = STT_WS_TASK_STACK,
        .task_name = "stt_ws",
        .task_prio = 5,
        .buffer_size = STT_WS_BUFFER_SIZE,
        /*
         * Reconnect after errors and after a close by the server (e.g. maximum session duration); a session per
         * turn is not reconnected (its audio is lost with it)
         */
        .disable_auto_reconnect = s_backend && s_backend->session_per_turn,
        .enable_close_reconnect = !(s_backend && s_backend->session_per_turn),
        .reconnect_timeout_ms = STT_RECONNECT_MS,
        .network_timeout_ms = 10000,
    };
    esp_websocket_client_handle_t client = esp_websocket_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to create WebSocket client");
        return NULL;
    }
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    if (esp_websocket_client_start(client) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client");
        esp_websocket_client_destroy(client);
        return NULL;
    }
    return client;
}

static void destroy_client(esp_websocket_client_handle_t client)
{
    /* No closing handshake: the server does not close the TCP connection after it (~1 s timeout) */
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
    xEventGroupClearBits(s_events, EV_CONNECTED);
}

static bool send_json(esp_websocket_client_handle_t client, const char *text, size_t len)
{
    return esp_websocket_client_send_text(client, text, len, pdMS_TO_TICKS(5000)) == (int)len;
}

/* The service's session configuration, if it has one */
static bool send_session_update(esp_websocket_client_handle_t client)
{
    char *text = s_backend->session_update ? s_backend->session_update(s_model, s_language) : NULL;
    bool ok = !s_backend->session_update || (text && send_json(client, text, strlen(text)));
    free(text);
    return ok;
}

static bool send_simple(esp_websocket_client_handle_t client, const char *type)
{
    if (!type) {
        return true;
    }
    char text[64];
    int len = snprintf(text, sizeof(text), "{\"type\":\"%s\"}", type);
    return send_json(client, text, len);
}

static bool send_audio(esp_websocket_client_handle_t client, const int16_t *samples, size_t count)
{
    // 24 kHz for OpenAI, the 16 kHz of the AFE as is for Mistral and Deepgram
    const int16_t *pcm = samples;
    size_t n_out = count;
    if (s_backend->rate != STT_INPUT_RATE) {
        n_out = upsample(&s_upsampler, samples, count, s_out);
        pcm = s_out;
    }
    if (!s_backend->append_type) {
        // The samples as they are, in a binary frame
        int len = (int)(n_out * sizeof(int16_t));
        return esp_websocket_client_send_bin(client, (const char *)pcm, len, pdMS_TO_TICKS(5000)) == len;
    }
    char prefix[64];
    size_t prefix_len = snprintf(prefix, sizeof(prefix), "{\"type\":\"%s\",\"audio\":\"", s_backend->append_type);
    static const char suffix[] = "\"}";
    size_t b64_len = 0;
    mbedtls_base64_encode(NULL, 0, &b64_len, (const unsigned char *)pcm, n_out * sizeof(int16_t));
    size_t total = prefix_len + b64_len + sizeof(suffix);
    if (total > s_message_cap) {
        char *p = heap_caps_realloc(s_message, total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) {
            return false;
        }
        s_message = p;
        s_message_cap = total;
    }
    memcpy(s_message, prefix, prefix_len);
    size_t written = 0;
    mbedtls_base64_encode((unsigned char *)s_message + prefix_len, b64_len, &written, (const unsigned char *)pcm,
                          n_out * sizeof(int16_t));
    memcpy(s_message + prefix_len + written, suffix, sizeof(suffix));
    return send_json(client, s_message, prefix_len + written + sizeof(suffix) - 1);
}

static void drain_audio(void)
{
    int16_t discard[64];
    while (xStreamBufferReceive(s_audio, discard, sizeof(discard), 0) > 0) {
    }
}

static void fail_turn(const char *reason)
{
    ESP_LOGE(TAG, "Turn failed: %s", reason);
    s_capturing = false;
    s_awaiting = false;
    s_turn_active = false;
    drain_audio();
    xEventGroupClearBits(s_events, EV_COMMIT);
    xEventGroupSetBits(s_events, EV_FAILED);
}

static void stt_task(void *arg)
{
    /* Nothing to do before the network is up */
    while (WIFI_STATUS_CONNECTED_OK != wifi_connected_already()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // A session per turn is opened when a turn starts
    esp_websocket_client_handle_t client = s_backend->session_per_turn ? NULL : create_client();
    bool session_ready = false;             /* connected and session.update sent */
    int64_t down_since = esp_timer_get_time();
    int64_t end_of_speech_us = 0;
    size_t backlog_at_end = 0;

    for (;;) {
        /* Another service, model or key: a new connection (the turn in progress, if any, is lost) */
        if (s_reconfigure) {
            if (client) {
                destroy_client(client);
            }
            if (s_turn_active) {
                fail_turn("transcription service changed");
            }
            session_ready = false;
            client = s_backend->session_per_turn ? NULL : create_client();
            down_since = esp_timer_get_time();
            ESP_LOGI(TAG, "%s: %s", s_backend->name, s_model);
            s_reconfigure = false;      // done: stt_stream_configure() returns
        }
        /* A session per turn: closed once its turn is over, opened again with the next */
        if (s_backend->session_per_turn && client && !s_turn_active && !s_awaiting) {
            destroy_client(client);
            client = NULL;
            session_ready = false;
        }
        /* New language: the session configuration is sent again (no reconnection needed) */
        if (s_language_change) {
            s_language_change = false;
            replace_str(&s_language, s_pending_language);
            session_ready = false;
            ESP_LOGI(TAG, "language: %s", s_language);
        }
        /* ----- connection supervision ----- */
        bool connected = client && esp_websocket_client_is_connected(client) &&
                         (xEventGroupGetBits(s_events) & EV_CONNECTED);
        if (connected && !session_ready) {
            if (send_session_update(client)) {
                session_ready = true;
                ESP_LOGI(TAG, "connected in %lld ms", (esp_timer_get_time() - down_since) / 1000);
            }
        } else if (!connected && session_ready) {
            session_ready = false;
            down_since = esp_timer_get_time();
            ESP_LOGW(TAG, "connection lost, reconnecting");
            /* Audio already sent in this turn is lost with the server session */
            if (s_turn_active && (s_turn_sent_samples > 0 || s_awaiting)) {
                fail_turn("connection lost during the question");
            }
        }
        if (!connected && (client || !s_backend->session_per_turn) &&
                esp_timer_get_time() - down_since > STT_RECREATE_AFTER_MS * 1000LL) {
            ESP_LOGW(TAG, "still disconnected, recreating the WebSocket client");
            if (client) {
                destroy_client(client);
            }
            client = (WIFI_STATUS_CONNECTED_OK == wifi_connected_already()) ? create_client() : NULL;
            down_since = esp_timer_get_time();
        }

        /* ----- commands (taken atomically) ----- */
        EventBits_t commands = xEventGroupClearBits(s_events, EV_TURN_START | EV_ABORT);
        if (commands & EV_ABORT) {
            s_turn_active = false;
            s_awaiting = false;
            drain_audio();
            xEventGroupClearBits(s_events, EV_COMMIT);
            if (session_ready) {
                send_simple(client, s_backend->clear_type);
            }
            continue;
        }
        if (commands & EV_TURN_START) {
            s_partial_len = 0;
            s_upsampler = (upsampler_t) {0};
            s_overflow_logged = false;
            s_turn_sent_samples = 0;
            end_of_speech_us = 0;
            /* Leftovers of an abandoned turn must not prefix this question (a new session is empty) */
            if (session_ready) {
                send_simple(client, s_backend->clear_type);
            }
            /* A session per turn: opened now, the audio waits in the buffer */
            if (s_backend->session_per_turn && !client) {
                down_since = esp_timer_get_time();
                client = create_client();
            }
        }

        /* ----- result timeout ----- */
        if (s_awaiting && esp_timer_get_time() - s_commit_us > STT_RESULT_TIMEOUT_MS * 1000LL) {
            fail_turn("no transcript");
            continue;
        }

        /* ----- audio of the current turn ----- */
        if (!s_turn_active || s_awaiting || !session_ready) {
            vTaskDelay(pdMS_TO_TICKS(STT_POLL_MS));
            continue;
        }
        EventBits_t bits = xEventGroupGetBits(s_events);
        if ((bits & EV_COMMIT) && !end_of_speech_us) {
            end_of_speech_us = esp_timer_get_time();
            backlog_at_end = xStreamBufferBytesAvailable(s_audio) / sizeof(int16_t);
        }
        /* Before the end of speech, wait for at least STT_MIN_CHUNK_SAMPLES (trigger level) */
        xStreamBufferSetTriggerLevel(s_audio, (bits & EV_COMMIT) ? sizeof(int16_t)
                                     : STT_MIN_CHUNK_SAMPLES * sizeof(int16_t));
        size_t n = xStreamBufferReceive(s_audio, s_in, STT_MAX_CHUNK_SAMPLES * sizeof(int16_t),
                                        pdMS_TO_TICKS(STT_POLL_MS));
        if (n >= sizeof(int16_t)) {
            if (send_audio(client, s_in, n / sizeof(int16_t))) {
                s_turn_sent_samples += n / sizeof(int16_t);
            } else {
                fail_turn("audio not sent");
            }
        } else if ((bits & EV_COMMIT) && xStreamBufferIsEmpty(s_audio)) {
            xEventGroupClearBits(s_events, EV_COMMIT);
            s_commit_us = esp_timer_get_time();
            s_awaiting = true;
            if (!send_simple(client, s_backend->commit_type)) {
                fail_turn("commit not sent");
                continue;
            }
            ESP_LOGI(TAG, "committed %u ms of audio; at the end of speech %u ms were still to send, sent in %lld ms",
                     (unsigned)(s_turn_sent_samples * 1000 / STT_INPUT_RATE),
                     (unsigned)(backlog_at_end * 1000 / STT_INPUT_RATE), (s_commit_us - end_of_speech_us) / 1000);
        }
    }
}

/* ---------- public API ---------- */

esp_err_t stt_stream_init(void)
{
    ESP_RETURN_ON_FALSE(!s_events, ESP_ERR_INVALID_STATE, TAG, "Already initialized");
    s_events = xEventGroupCreate();
    s_audio = xStreamBufferCreateWithCaps(STT_AUDIO_BUFFER_BYTES, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_in = heap_caps_malloc(STT_MAX_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_out = heap_caps_malloc(STT_CHUNK_OUT_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_events && s_audio && s_in && s_out, ESP_ERR_NO_MEM, TAG, "Out of memory");
    return ESP_OK;
}

void stt_stream_set_language(const char *language)
{
    strlcpy(s_pending_language, language, sizeof(s_pending_language));
    s_language_change = true;
}

void stt_stream_configure(const stt_backend_t *backend, const char *api_key, const char *model,
                          const char *language)
{
    char *header = NULL;
    asprintf(&header, "Authorization: %s %s\r\n", backend->auth_scheme ? backend->auth_scheme : "Bearer", api_key);
    // Taken by the stt task at its next loop (which makes the URI): the old header stays valid until it has
    // reconnected
    char *old_header = s_auth_header;
    s_auth_header = header;
    replace_str(&s_model, model);
    if (language) {
        replace_str(&s_language, language);
    }
    bool change = (s_backend != NULL);
    s_backend = backend;
    ESP_LOGI(TAG, "%s model: %s, language: %s", backend->name, s_model ? s_model : "-",
             s_language ? s_language : "-");
    if (change) {
        // Applied by the stt task (closing the old connection can take a second): a turn started before that
        // would be lost with it
        s_reconfigure = true;
        for (int i = 0; i < 100 && s_reconfigure; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    free(old_header);

    /* The connection task starts once configured; it connects as soon as the Wi-Fi is up */
    static bool task_started = false;
    if (!task_started) {
        task_started = (pdPASS == xTaskCreatePinnedToCoreWithCaps(stt_task, "stt", 6 * 1024, NULL, 5, NULL, 0,
                                                                   MALLOC_CAP_SPIRAM));
        if (!task_started) {
            ESP_LOGE(TAG, "Failed to create task");
        }
    }
}

void stt_stream_start(stt_partial_cb_t on_partial)
{
    /* A previous turn still waiting for its transcript is abandoned */
    xEventGroupClearBits(s_events, EV_COMMIT | EV_TRANSCRIPT | EV_FAILED);
    s_awaiting = false;
    s_on_partial = on_partial;
    free(s_transcript);
    s_transcript = NULL;
    s_turn_active = true;
    s_capturing = true;
    xEventGroupSetBits(s_events, EV_TURN_START);
}

void stt_stream_write(const int16_t *samples, size_t count)
{
    if (!s_capturing) {
        return;
    }
    size_t bytes = count * sizeof(int16_t);
    if (xStreamBufferSend(s_audio, samples, bytes, 0) != bytes && !s_overflow_logged) {
        s_overflow_logged = true;
        ESP_LOGW(TAG, "Audio buffer full: audio dropped");
    }
}

char *stt_stream_finish(uint32_t timeout_ms)
{
    s_capturing = false;
    xEventGroupSetBits(s_events, EV_COMMIT);
    EventBits_t bits = xEventGroupWaitBits(s_events, EV_TRANSCRIPT | EV_FAILED, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    char *text = NULL;
    if (bits & EV_TRANSCRIPT) {
        text = s_transcript;
        s_transcript = NULL;
    } else {
        ESP_LOGE(TAG, "No transcript (%s)", (bits & EV_FAILED) ? "error" : "timeout");
        xEventGroupSetBits(s_events, EV_ABORT);
    }
    return text;
}

char *stt_stream_test(const int16_t *audio, size_t samples, int *ms)
{
    *ms = -1;
    if (!stt_stream_wait_connected(s_backend && s_backend->session_per_turn ? 0 : 10000) &&
            !(s_backend && s_backend->session_per_turn)) {
        return NULL;
    }
    stt_stream_start(NULL);
    // A session per turn: opened now, measured from when it is up (as when the user speaks meanwhile)
    if (s_backend->session_per_turn && !stt_stream_wait_connected(10000)) {
        stt_stream_abort();
        return NULL;
    }
    vTaskDelay(pdMS_TO_TICKS(300));     // the session configuration sent
    size_t max = STT_AUDIO_BUFFER_BYTES / sizeof(int16_t) - 16;
    if (samples > max) {
        samples = max;
    }
    // Sent at the pace of speech (16 kHz, 100 ms blocks), as from the microphone: a realtime model transcribes as
    // the audio comes, and given all at once it would still be working through it at the commit
    for (size_t pos = 0; pos < samples; pos += 1600) {
        stt_stream_write(audio + pos, samples - pos < 1600 ? samples - pos : 1600);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    // Measured from the commit (the audio all sent), as after the end of speech in use
    s_last_final_ms = -1;
    char *text = stt_stream_finish(20000);
    *ms = text ? s_last_final_ms : -1;
    return text;
}

bool stt_stream_no_credit(void)
{
    return s_no_credit;
}

bool stt_stream_has_text(void)
{
    for (size_t i = 0; i < s_partial_len; i++) {
        if ((unsigned char)s_partial[i] > ' ') {
            return true;
        }
    }
    return false;
}

bool stt_stream_wait_connected(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(s_events, EV_CONNECTED, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return bits & EV_CONNECTED;
}

void stt_stream_abort(void)
{
    s_capturing = false;
    xEventGroupSetBits(s_events, EV_ABORT);
}
