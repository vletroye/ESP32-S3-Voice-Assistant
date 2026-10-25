/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "app_ui_ctrl.h"
#include "app_sr.h"
#include "bsp/esp-bsp.h"
#include "bsp_board.h"
#include "app_audio.h"
#include "app_wifi.h"
#include "settings.h"
#include "app_lang.h"
#include "llm.h"
#include "app_memory.h"
#include "sentence_splitter.h"
#include "tts_stream.h"
#include "stt_stream.h"
#include "cJSON.h"

#define LISTEN_SPEAK_PANEL_DELAY_MS     2000
/* Maximum wait for the final transcript after the end of speech */
#define STT_FINAL_TIMEOUT_MS            8000

extern bool record_flag;

static char *TAG = "app_main";
static sys_param_t *sys_param = NULL;
/* Sentences of the answer being streamed, sent to speech synthesis as soon as complete */
static sentence_splitter_t s_splitter;
/* End of transcription, to measure the time to the first spoken audio */
static int64_t s_reply_t0 = 0;
/* A reply has been spoken: the conversation is saved by the main task (flash writes pause the audio) */
static volatile bool s_history_save_pending = false;

/* Answer text shown on screen while the model streams it */
#define REPLY_UI_REFRESH_US     (200 * 1000)

typedef struct {
    char *text;
    size_t len;
    size_t cap;
    int64_t last_refresh_us;
    bool shown;
} reply_view_t;

static void reply_view_append(reply_view_t *v, const char *s)
{
    size_t n = strlen(s);
    if (v->len + n + 1 > v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 512;
        while (cap < v->len + n + 1) {
            cap *= 2;
        }
        char *p = realloc(v->text, cap);
        if (!p) {
            return;
        }
        v->text = p;
        v->cap = cap;
    }
    memcpy(v->text + v->len, s, n + 1);
    v->len += n;
}

static void reply_view_refresh(reply_view_t *v)
{
    if (!v->text) {
        return;
    }
    ui_ctrl_label_show_text(UI_CTRL_LABEL_REPLY_CONTENT, v->text);
    if (!v->shown) {
        ui_ctrl_show_panel(UI_CTRL_PANEL_REPLY, 0);
        v->shown = true;
    }
    v->last_refresh_us = esp_timer_get_time();
}

/* Streaming callback: refresh the screen at most every REPLY_UI_REFRESH_US */
static void on_llm_text(const char *delta, void *ctx)
{
    reply_view_t *v = (reply_view_t *)ctx;
    sentence_splitter_feed(&s_splitter, delta);
    reply_view_append(v, delta);
    if (!v->shown || esp_timer_get_time() - v->last_refresh_us >= REPLY_UI_REFRESH_US) {
        reply_view_refresh(v);
    }
}

static int64_t elapsed_ms(int64_t start_us)
{
    return (esp_timer_get_time() - start_us) / 1000;
}

/*
 * Whisper "hallucinates" these subtitle credits when it is given silence or noise.
 * Matched case-insensitively as substrings of the transcription.
 */
static const char *const s_noise_transcriptions[] = {
    "amara.org",
    "sous-titres réalisés",
    "sous-titrage",
    "sous-titres par",
    "merci d'avoir regardé",
    "subtitles by",
    "thanks for watching",
    "thank you for watching",
};

static bool transcription_is_noise(const char *text)
{
    size_t letters = 0;
    for (const char *p = text; *p; p++) {
        if (isalnum((unsigned char)*p) || ((unsigned char)*p & 0x80)) {
            letters++;
        }
    }
    if (letters < 2) {
        return true;
    }

    for (size_t i = 0; i < sizeof(s_noise_transcriptions) / sizeof(s_noise_transcriptions[0]); i++) {
        if (strcasestr(text, s_noise_transcriptions[i])) {
            return true;
        }
    }
    return false;
}

/* "Au revoir" alone (punctuation and case ignored) ends the conversation */
static bool transcription_is_goodbye(const char *text)
{
    const char *const *goodbyes = app_lang_get()->goodbyes;
    if (goodbyes == NULL) {
        return false;
    }
    char norm[48];
    app_text_normalize(text, norm, sizeof(norm));
    for (; *goodbyes; goodbyes++) {
        if (strcmp(norm, *goodbyes) == 0) {
            return true;
        }
    }
    return false;
}

static void on_sentence(const char *sentence, void *ctx)
{
    tts_stream_say(sentence);
}

/* Called from the playback task */
static void on_reply_audio(tts_event_t event)
{
    if (event == TTS_EVENT_FIRST_AUDIO) {
        ESP_LOGI(TAG, "[timing] first audio %lld ms after the transcription", elapsed_ms(s_reply_t0));
        ui_ctrl_reply_set_audio_start_flag(true);
        return;
    }

    // TTS_EVENT_DONE: the whole reply has been spoken
    ESP_LOGI(TAG, "[timing] reply spoken, %lld ms after the transcription", elapsed_ms(s_reply_t0));
    s_history_save_pending = true;
    if (record_flag) {
        // Reply interrupted by a new listening session (wake word or touch)
        return;
    }
#if CONFIG_ASSISTANT_FOLLOWUP_WINDOW_MS > 0
    // Keep listening so that the conversation can continue without the wake word
    app_sr_start_followup(CONFIG_ASSISTANT_FOLLOWUP_WINDOW_MS);
#else
    ui_ctrl_reply_set_audio_end_flag(true);
#endif
}

/* Stop the reply being spoken. Called from app_audio.c when a new listening session starts. */
void assistant_reply_stop(void)
{
    tts_stream_cancel();
}

/* Show and speak a message as the reply (errors, memory commands), sentence by sentence */
static void speak_message(const char *message)
{
    ui_ctrl_label_show_text(UI_CTRL_LABEL_REPLY_CONTENT, message);
    ui_ctrl_show_panel(UI_CTRL_PANEL_REPLY, 0);
    s_reply_t0 = esp_timer_get_time();
    tts_stream_begin(on_reply_audio);
    sentence_splitter_init(&s_splitter, on_sentence, NULL);
    sentence_splitter_feed(&s_splitter, message);
    sentence_splitter_finish(&s_splitter);
    tts_stream_end();
}

/*
 * Instructions given to the model: the base ones, the Persona of PERSONA.TXT and the notes the user asked
 * to remember (MEMORY.TXT). Allocated, to be freed.
 */
static char *build_system_prompt(void)
{
    const app_lang_t *lang = app_lang_get();
    const char *notes = app_memory_notes();
    const char *persona = sys_param->persona ? sys_param->persona : "";
    char *prompt = NULL;
    asprintf(&prompt, "%s%s%s%s%s%s%s", lang->system_prompt,
             persona[0] ? "\n\n" : "", persona,
             notes[0] ? "\n\n" : "", notes[0] ? lang->memory_header : "", notes[0] ? "\n" : "", notes);
    return prompt;
}

/* Start of a listening session (wake word, touch or follow-up). Called from app_audio.c */
void assistant_listen_start(void)
{
    // The transcript is not shown while the user speaks: the question appears above the answer
    stt_stream_start(NULL);
}

/* Open the chat connection while the user speaks (speech uses a permanent WebSocket). Called from app_audio.c */
void assistant_prewarm(void)
{
    /* The transcription connection is the most urgent: let its TLS handshake finish first */
    stt_stream_wait_connected(3000);
    llm_prewarm();
}

/* Nobody spoke: drop the transcription session. Called from app_audio.c */
void assistant_listen_abort(void)
{
    stt_stream_abort();
}

/* End of speech: get the final transcript, then answer. Called from app_audio.c (SR handler task) */
void assistant_listen_end(void)
{
    esp_err_t ret = ESP_OK;
    char *response = NULL;
    int64_t t_start;

    ui_ctrl_show_panel(UI_CTRL_PANEL_GET, 0);

    t_start = esp_timer_get_time();
    char *text = stt_stream_finish(STT_FINAL_TIMEOUT_MS);
    ESP_LOGI(TAG, "[timing] final transcript %lld ms after the end of speech", elapsed_ms(t_start));
    if (NULL == text) {
        speak_message(app_lang_get()->stt_error);
        return;
    }

    if (transcription_is_noise(text)) {
        ESP_LOGW(TAG, "Ignored transcription (silence or noise): \"%s\"", text);
        ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, 0);
        goto err;
    }

    if (transcription_is_goodbye(text)) {
        // Back to sleep: no answer, no follow-up listening, the next conversation starts afresh
        ESP_LOGI(TAG, "Goodbye: \"%s\", going to sleep", text);
        llm_reset();
        ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, 0);
        goto err;
    }

    // UI listen success
    ui_ctrl_label_show_text(UI_CTRL_LABEL_REPLY_QUESTION, text);

    // "Souviens-toi que ...", "oublie que ...", "oublie tout", "dis-moi ce que tu as mémorisé": answered here
    char *memory_reply = NULL;
    memory_cmd_t memory_cmd = app_memory_handle(text, &memory_reply);
    if (memory_cmd != MEMORY_CMD_NONE) {
        if (memory_cmd == MEMORY_CMD_CHANGED) {
            char *prompt = build_system_prompt();
            if (prompt) {
                llm_set_system_prompt(prompt);
                free(prompt);
            }
        }
        speak_message(memory_reply ? memory_reply : "");
        free(memory_reply);
        goto err;
    }

    // The answer is streamed and shown as it arrives, prefixed with the model name (not spoken)
    reply_view_t view = {0};
    reply_view_append(&view, "[");
    reply_view_append(&view, sys_param->chat_model);
    reply_view_append(&view, "] ");
    size_t prefix_len = view.len;

    // Each sentence is spoken as soon as the model has written it
    s_reply_t0 = esp_timer_get_time();
    tts_stream_begin(on_reply_audio);
    sentence_splitter_init(&s_splitter, on_sentence, NULL);

    t_start = esp_timer_get_time();
    llm_error_t llm_err = llm_stream(text, on_llm_text, &view, &response);
    ESP_LOGI(TAG, "[timing] chat (%s, streamed): %lld ms", llm_provider_name(llm_provider()), elapsed_ms(t_start));

    if (llm_err == LLM_OK) {
        sentence_splitter_finish(&s_splitter);
    } else {
        // The error is shown and spoken as if it were the answer (replacing any partial text)
        sentence_splitter_discard(&s_splitter);
        tts_stream_begin(on_reply_audio);
        const char *message = app_lang_get()->llm_errors[llm_provider()][llm_err];
        tts_stream_say(message);
        free(response);
        response = strdup(message);
        if (view.text) {
            view.text[prefix_len] = '\0';
            view.len = prefix_len;
        }
        reply_view_append(&view, response ? response : "");
        ESP_LOGW(TAG, "Chat error %d: speaking \"%s\"", llm_err, response ? response : "");
    }
    reply_view_refresh(&view);
    free(view.text);

    tts_stream_end();

    if (NULL == response) {
        ret = ESP_ERR_NO_MEM;
        ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, LISTEN_SPEAK_PANEL_DELAY_MS);
        ESP_GOTO_ON_ERROR(ret, err, TAG, "[chat]: out of memory");
    }
    ui_ctrl_label_show_text(UI_CTRL_LABEL_REPLY_QUESTION, text);

err:
    // Clearing resources

    free(response);

    if (text) {
        free(text);
    }
    (void)ret;
}



static void *psram_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void app_main()
{
    // JSON trees (conversation history, API events) go to PSRAM: internal RAM is kept for stacks and DMA
    cJSON_Hooks json_hooks = {
        .malloc_fn = psram_malloc,
        .free_fn = free,
    };
    cJSON_InitHooks(&json_hooks);

    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(settings_read_parameter_from_nvs());
    sys_param = settings_get_parameter();
    app_lang_set(sys_param->lang);

    bsp_spiffs_mount();
    bsp_i2c_init();

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = 0,
        .flags = {
            .buff_dma = true,
        }
    };
    bsp_display_start_with_config(&cfg);
    bsp_board_init();
    /*
     * Codec output set once: writing its registers while a sound plays out (before a beep or a reply)
     * makes crackles
     */
    bsp_codec_mute_set(false);
    bsp_codec_volume_set(CONFIG_VOLUME_LEVEL, NULL);

    ESP_LOGI(TAG, "Display LVGL demo");
    bsp_display_backlight_on();
    ui_ctrl_init();
    app_network_start();

    ESP_LOGI(TAG, "speech recognition start");
    app_sr_start(false);

    // The Persona (name, who made it, who it talks to...) and the remembered notes complete the base instructions
    app_memory_init(sys_param->memory);
    char *system_prompt = build_system_prompt();
    llm_init(sys_param->chat_model, sys_param->claude_key, sys_param->key, sys_param->url,
             system_prompt ? system_prompt : app_lang_get()->system_prompt);
    free(system_prompt);
    ESP_ERROR_CHECK(stt_stream_init());
    stt_stream_configure(sys_param->key, CONFIG_ASSISTANT_STT_MODEL, app_lang_get()->code);
    ESP_ERROR_CHECK(tts_stream_init());
    tts_stream_configure(sys_param->url, sys_param->key, CONFIG_ASSISTANT_TTS_MODEL, sys_param->voice,
                         app_lang_get()->tts_instructions);

    for (int seconds = 0; ; seconds++) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Conversation saved after each spoken reply (this task's stack is in internal RAM, as flash writes need)
        if (s_history_save_pending) {
            s_history_save_pending = false;
            llm_history_save();
        }
        if (seconds % 30) {
            continue;
        }
        // Internal RAM is the scarce resource (task stacks, DMA); PSRAM holds the large buffers
        ESP_LOGI(TAG, "[heap] internal free %u (min %u, largest block %u), PSRAM free %u (min %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    }
}
