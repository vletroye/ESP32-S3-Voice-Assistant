/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "driver/usb_serial_jtag.h"
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
#include "voice_cmd.h"
#include "phrases.h"
#include "openrouter_free.h"
#include "app_presence.h"
#include "assistant.h"
#include "app_volume.h"
#include "tools.h"
#include "weather.h"
#include "homey_mcp.h"
#include "sentence_splitter.h"
#include "tts_stream.h"
#include "stt_stream.h"
#include "cJSON.h"
#include "model_test.h"
#include "costs.h"
#include "persona.h"
#include "country.h"

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
/*
 * Flash writes pause the tasks running from flash (microphone feed included): the conversation is saved once
 * nothing has been heard or played for this long, i.e. after the follow-up listening, not during it
 */
#define SAVE_AFTER_SILENCE_MS           1000
/* A language added to Languages: its message stays this long before the configuration mode */
#define NEW_LANGUAGE_MESSAGE_MS         6000
/* Homey's tools were added */
static bool homey_loaded = false;
/* From the start of an answer until it has been spoken or interrupted (no flash write meanwhile) */
static volatile bool s_reply_active = false;
/* "Édite la config": configuration mode once the reply has been spoken (set by the reply, done by the main task) */
static volatile bool s_config_after_reply = false;
static volatile bool s_config_requested = false;
/* "Redémarre": restart once the reply has been spoken (set by the reply, done by the main task) */
static volatile bool s_restart_after_reply = false;
static volatile bool s_restart_requested = false;
/* "Teste les modèles": the test starts once its announcement has been spoken */
static volatile bool s_test_after_reply = false;
/* The reply is the one of a voice command handled here (not of the model): Sleep_after_cmd applies */
static volatile bool s_reply_local_cmd = false;
/* The reply ended with a goodbye ("Au revoir !", "Bonne soirée."): sleep and a new conversation once spoken */
static volatile bool s_reply_goodbye = false;
static volatile bool s_reset_requested = false;

/* Answer text shown on screen while the model streams it */
#define REPLY_UI_REFRESH_US     (200 * 1000)

typedef struct {
    char *text;
    size_t len;
    size_t cap;
    int64_t last_refresh_us;
    bool shown;
    size_t part_start;      /* start of the text written since the last tool call */
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
static void on_sentence(const char *sentence, void *ctx);
static void speak_message(const char *message);

static void on_llm_text(const char *delta, void *ctx)
{
    reply_view_t *v = (reply_view_t *)ctx;
    if (delta == NULL) {
        // A tool runs (web search...): speak what was written so far, even a short "Je regarde.", but take
        // this waiting sentence off the screen, which keeps only the answer
        sentence_splitter_finish(&s_splitter);
        sentence_splitter_init(&s_splitter, on_sentence, NULL);
        if (llm_web_searches() > 0) {
            ui_ctrl_reply_set_logo(UI_CTRL_LOGO_WEB);
        }
        if (v->text && v->len > v->part_start) {
            v->len = v->part_start;
            v->text[v->len] = '\0';
            reply_view_refresh(v);
        }
        return;
    }
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

/* Each sentence of the reply is spoken; the last one tells whether the reply says goodbye (s_goodbyes) */
static void on_sentence(const char *sentence, void *ctx)
{
    s_reply_goodbye = voice_cmd_ends_with_goodbye(sentence);
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
    s_reply_active = false;
    if (tts_stream_reply_was_choppy()) {
        // Speech cut or sentences lost: the answer stays readable on screen, say why it was heard badly
        ui_ctrl_reply_append_notice(app_lang_get()->voice_choppy);
    }
    voice_cmd_reply_spoken();
    if (s_restart_after_reply) {
        s_restart_after_reply = false;
        s_restart_requested = true;     // the main task saves and restarts
        return;
    }
    if (s_config_after_reply) {
        s_config_after_reply = false;
        s_config_requested = true;      // the main task saves and restarts
        return;
    }
    if (s_test_after_reply) {
        s_test_after_reply = false;
        model_test_start();             // run by the main task; no listening meanwhile
        return;
    }
    // A volume asked during the reply ("parle plus fort"): the codec can be written now that it is silent
    app_volume_apply_pending();
    if (record_flag) {
        // Reply interrupted by a new listening session (wake word or touch)
        return;
    }
    if (s_reply_local_cmd && sys_param->sleep_after_cmd && !voice_cmd_waiting_answer()) {
        // Sleep_after_cmd: a voice command handled here (and asking nothing) does not listen again
        ESP_LOGI(TAG, "Voice command done, going to sleep (Sleep_after_cmd)");
        ui_ctrl_reply_set_audio_end_flag(true);
        return;
    }
    if (s_reply_goodbye) {
        // The assistant said goodbye: no follow-up listening, the next conversation starts afresh
        ESP_LOGI(TAG, "The reply says goodbye, going to sleep");
        s_reply_goodbye = false;
        s_reset_requested = true;       // the main task clears the conversation
        ui_ctrl_reply_set_audio_end_flag(true);
        return;
    }
#if CONFIG_ASSISTANT_FOLLOWUP_WINDOW_MS > 0
    if (!tools_sleep_after_reply()) {
        // Keep listening so that the conversation can continue without the wake word
        app_sr_start_followup(CONFIG_ASSISTANT_FOLLOWUP_WINDOW_MS);
        return;
    }
#endif
    // Back to sleep once the answer has scrolled (no follow-up, or a volume change)
    ui_ctrl_reply_set_audio_end_flag(true);
}

/* Stop the reply being spoken. Called from app_audio.c when a new listening session starts. */
void assistant_reply_stop(void)
{
    tts_stream_cancel();
    s_reply_active = false;
    s_test_after_reply = false;
    model_test_cancel();
}

void assistant_show(const char *text)
{
    ui_ctrl_reply_set_logo(UI_CTRL_LOGO_LOCAL);
    ui_ctrl_reply_show_end(text);
    ui_ctrl_show_panel(UI_CTRL_PANEL_REPLY, 0);
}

static volatile bool s_announced = false;

static void on_announce_audio(tts_event_t event)
{
    if (event == TTS_EVENT_DONE) {
        s_announced = true;
    }
}

void assistant_announce_begin(void)
{
    s_announced = false;
    tts_stream_begin(on_announce_audio);
}

void assistant_announce(const char *text)
{
    tts_stream_say(text);
}

void assistant_announce_end(void)
{
    tts_stream_end();
    int64_t t0 = esp_timer_get_time();
    while (!s_announced && !model_test_cancelled() && esp_timer_get_time() - t0 < 60 * 1000 * 1000LL) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void assistant_say(const char *text)
{
    speak_message(text);
}

void assistant_test_done(void)
{
    ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, 0);
}

/* Start a spoken answer (its sentences follow with tts_stream_say) */
static void reply_begin(void)
{
    s_reply_active = true;
    s_reply_local_cmd = false;
    s_reply_goodbye = false;
    tts_stream_begin(on_reply_audio);
}

/* Show and speak a message as the reply (errors, memory commands), sentence by sentence */
static void speak_message(const char *message)
{
    ui_ctrl_reply_set_logo(UI_CTRL_LOGO_LOCAL);
    ui_ctrl_label_show_text(UI_CTRL_LABEL_REPLY_CONTENT, message);
    ui_ctrl_show_panel(UI_CTRL_PANEL_REPLY, 0);
    s_reply_t0 = esp_timer_get_time();
    reply_begin();
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
    // "Lieu de l'utilisateur : Evere, Belgique." (country and town from CONFIG.INI or "change mon pays / ma ville";
    // the country named in the language in use, COUNTRY.TXT)
    char where[160] = "";
    char location[256] = "";
    char country[64] = "";
    if (sys_param->country[0]) {
        country_name(sys_param->country, country, sizeof(country));
    }
    snprintf(where, sizeof(where), "%s%s%s", sys_param->city, sys_param->city[0] && country[0] ? ", " : "", country);
    if (where[0]) {
        snprintf(location, sizeof(location), lang->location_line, where);
    }
    const char *home = sys_param->homey_url[0] ? lang->home_line : "";
    // Identity, personality and instructions of PERSO_<LANG>.TXT, then what the working requires (app_lang.c),
    // the location, the home and the notes to remember
    const char *parts[] = {
        persona_get(PERSONA_IDENTITY), persona_get(PERSONA_PERSONALITY), persona_get(PERSONA_INSTRUCTIONS),
        lang->system_prompt, location, home,
    };
    char *prompt = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&prompt, &size);
    if (!out) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        if (parts[i][0]) {
            fprintf(out, "%s%s", ftell(out) ? "\n\n" : "", parts[i]);
        }
    }
    if (notes[0]) {
        fprintf(out, "\n\n%s\n%s", lang->memory_header, notes);
    }
    fclose(out);
    return prompt;
}

/* Instructions of the speech synthesis: read word for word (app_lang.c), then the [voice] of PERSO_<LANG>.TXT */
static char *build_tts_instructions(void)
{
    char *text = NULL;
    const char *voice = persona_get(PERSONA_VOICE);
    asprintf(&text, "%s%s%s", app_lang_get()->tts_instructions, voice[0] ? " " : "", voice);
    return text;
}

/*
 * The question as sent to the model, starting with the current date and time ("[Nous sommes le dimanche
 * 25 octobre 2026, il est 20 h 07.]"): in the user message rather than in the instructions, which stay
 * identical and cached. NULL when the clock is not synchronized yet. Allocated, to be freed.
 */
static char *with_current_time(const char *text)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year + 1900 < 2024) {
        return NULL;
    }
    const app_lang_t *lang = app_lang_get();
    char stamp[96];
    snprintf(stamp, sizeof(stamp), lang->now_format, lang->weekdays_long[t.tm_wday], t.tm_mday,
             lang->months[t.tm_mon], t.tm_year + 1900, t.tm_hour, t.tm_min);
    char *question = NULL;
    asprintf(&question, "%s %s", stamp, text);
    return question;
}

/* Changes asked from the Settings screen, applied by the main task (assistant_request_...) */
static char s_requested_language[LANG_SIZE];
static char s_requested_model[MODEL_SIZE];
static volatile bool s_language_requested = false;
static volatile bool s_model_requested = false;
static volatile bool s_restore_requested = false;
static volatile bool s_factory_requested = false;

void assistant_request_restore(bool factory)
{
    s_factory_requested = factory;
    s_restore_requested = true;
}

/* Files written by the assistant, removed by the factory reset (CONFIG.BKP kept, sounds of the image too) */
static const char *const s_user_files[] = {
    "history.json", "history.tmp", "places.json", "or_free.json", "record.wav", "model_times.json", "speed.txt", "log.txt",
    "costs.json", "costs.tmp", "costs.txt",
};

/* Languages whose files are given with the firmware: a soft reset restores them; those of a language added by the
 * user (its translation) are kept */
static const char *const s_firmware_languages[] = { "fr", "en" };

/* "phrases_de.txt" is a file of a language (prefix "phrases_", "lang_" or "persona_"), of the firmware's ones? */
static bool language_file(const char *name, bool *firmware, bool *persona)
{
    static const char *const prefixes[] = { "phrases_", "lang_", "persona_" };
    size_t len = strlen(name);
    for (size_t p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p++) {
        size_t n = strlen(prefixes[p]);
        if (strncmp(name, prefixes[p], n) == 0 && len > n + 4 && strcmp(name + len - 4, ".txt") == 0) {
            *firmware = false;
            for (size_t i = 0; i < sizeof(s_firmware_languages) / sizeof(s_firmware_languages[0]); i++) {
                *firmware |= (len - n - 4 == strlen(s_firmware_languages[i]) &&
                              strncmp(name + n, s_firmware_languages[i], len - n - 4) == 0);
            }
            *persona = (p == 2);
            return true;
        }
    }
    return false;
}

/*
 * The files of the configuration drive given with the firmware, removed: installed again at start-up; with
 * the factory reset, those written by the assistant (conversation, places, free models) too
 */
static void remove_firmware_files(bool factory)
{
    DIR *dir = opendir("/spiffs");
    struct dirent *e;
    while (dir && (e = readdir(dir)) != NULL) {
        // Soft reset: the phrase and text files of the firmware's languages (the persona is the user's); factory
        // reset: every file of every language
        bool firmware = false, persona = false;
        bool language = language_file(e->d_name, &firmware, &persona);
        bool user_file = false;
        for (size_t i = 0; factory && i < sizeof(s_user_files) / sizeof(s_user_files[0]); i++) {
            user_file |= (strcmp(e->d_name, s_user_files[i]) == 0);
        }
        if ((language && (factory || (firmware && !persona))) || user_file || strcmp(e->d_name, "agents.ini") == 0) {
            char path[300];
            snprintf(path, sizeof(path), "/spiffs/%s", e->d_name);
            ESP_LOGI(TAG, "%s %s", remove(path) == 0 ? "Removed" : "Could not remove", path);
        }
    }
    if (dir) {
        closedir(dir);
    }
    // The countries: removed by the factory reset; by the soft reset, kept aside so that the columns of the
    // languages added are taken back into the firmware's file
    remove(COUNTRY_OLD_FILE);
    if (factory) {
        ESP_LOGI(TAG, "%s %s", remove(COUNTRY_FILE) == 0 ? "Removed" : "Could not remove", COUNTRY_FILE);
    } else {
        ESP_LOGI(TAG, "%s %s", rename(COUNTRY_FILE, COUNTRY_OLD_FILE) == 0 ? "Kept aside" : "Could not keep aside",
                 COUNTRY_FILE);
    }
}

void assistant_request_language(const char *code)
{
    strlcpy(s_requested_language, code, sizeof(s_requested_language));
    s_language_requested = true;
}

void assistant_request_model(const char *model)
{
    strlcpy(s_requested_model, model, sizeof(s_requested_model));
    s_model_requested = true;
}

void assistant_set_language(const char *code)
{
    strlcpy(sys_param->lang, code, sizeof(sys_param->lang));
    app_lang_set(code);
    const app_lang_t *lang = app_lang_get();
    // Who the assistant is in this language (PERSO_<LANG>.TXT)
    persona_load(lang->code);
    stt_stream_set_language(lang->code);
    char *tts_instructions = build_tts_instructions();
    tts_stream_set_instructions(tts_instructions ? tts_instructions : lang->tts_instructions);
    free(tts_instructions);
    char *prompt = build_system_prompt();
    if (prompt) {
        llm_set_system_prompt(prompt);
        free(prompt);
    }
    ui_ctrl_apply_language();
    ESP_LOGI(TAG, "Language %s applied", code);
}

void assistant_set_location(const char *country, const char *city)
{
    if (country) {
        strlcpy(sys_param->country, country, sizeof(sys_param->country));
    }
    if (city) {
        strlcpy(sys_param->city, city, sizeof(sys_param->city));
    }
    llm_set_location(sys_param->country, sys_param->city);
    char *prompt = build_system_prompt();
    if (prompt) {
        llm_set_system_prompt(prompt);
        free(prompt);
    }
    ESP_LOGI(TAG, "Location: %s, %s", sys_param->city, sys_param->country);
}

void assistant_set_model(const char *model)
{
    // The current conversation is saved first: it is restored if this model is chosen again later
    llm_history_save();
    strlcpy(sys_param->chat_model, model, sizeof(sys_param->chat_model));
    char *prompt = build_system_prompt();
    llm_init(model, sys_param->claude_key, sys_param->key, sys_param->url,
             sys_param->gemini_key, sys_param->openrouter_key,
             prompt ? prompt : app_lang_get()->system_prompt);
    free(prompt);
    llm_set_location(sys_param->country, sys_param->city);
    ui_ctrl_settings_sync();
    ESP_LOGI(TAG, "Model %s applied", model);
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

    // The transcript is there: the answer is being prepared
    ui_ctrl_label_show_text(UI_CTRL_LABEL_LISTEN_SPEAK, app_lang_get()->thinking);

    if (voice_cmd_is_goodbye(text)) {
        // Back to sleep: no answer, no follow-up listening, the next conversation starts afresh
        ESP_LOGI(TAG, "Goodbye: \"%s\", going to sleep", text);
        llm_reset();
        ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, 0);
        goto err;
    }

    // UI listen success
    ui_ctrl_label_show_text(UI_CTRL_LABEL_REPLY_QUESTION, text);

    // "Change la langue" / "change de modèle" (then the answer), "parle anglais", "utilise le modèle 3":
    // answered and applied here, without restarting
    char *cmd_reply = NULL;
    voice_cmd_t voice_cmd = voice_cmd_handle(text, &cmd_reply);
    if (voice_cmd == VOICE_CMD_OPEN_SETTINGS) {
        // Shown at once, without a spoken reply (nor follow-up listening)
        ui_ctrl_open_settings();
        goto err;
    }
    if (voice_cmd == VOICE_CMD_NEW_CONVERSATION || voice_cmd == VOICE_CMD_NEW_CONVERSATION_ASK) {
        // "Nouvelle discussion", "j'ai une autre question": a fresh conversation, as after "au revoir"
        ESP_LOGI(TAG, "New conversation: \"%s\"", text);
        llm_reset();
    }
    if (voice_cmd != VOICE_CMD_NONE && voice_cmd != VOICE_CMD_NEW_CONVERSATION_ASK) {
        s_config_after_reply = (voice_cmd == VOICE_CMD_EDIT_CONFIG);
        s_restart_after_reply = (voice_cmd == VOICE_CMD_RESTART);
        s_test_after_reply = (voice_cmd == VOICE_CMD_TEST_MODELS);
        speak_message(cmd_reply ? cmd_reply : "");
        s_reply_local_cmd = true;
        free(cmd_reply);
        goto err;
    }

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
        s_reply_local_cmd = true;
        free(memory_reply);
        goto err;
    }

    // The answer is streamed and shown as it arrives, prefixed with the model name (not spoken)
    reply_view_t view = {0};
    reply_view_append(&view, "[");
    reply_view_append(&view, sys_param->chat_model);
    reply_view_append(&view, "] ");
    size_t prefix_len = view.len;
    view.part_start = prefix_len;

    // Each sentence is spoken as soon as the model has written it
    s_reply_t0 = esp_timer_get_time();
    reply_begin();
    sentence_splitter_init(&s_splitter, on_sentence, NULL);

    t_start = esp_timer_get_time();
    tools_reset_requests();
    ui_ctrl_reply_set_logo(llm_provider() == LLM_PROVIDER_OPENAI ? UI_CTRL_LOGO_OPENAI :
                           llm_provider() == LLM_PROVIDER_GEMINI ? UI_CTRL_LOGO_GEMINI :
                           llm_provider() == LLM_PROVIDER_OPENROUTER ? UI_CTRL_LOGO_OPENROUTER : UI_CTRL_LOGO_CLAUDE);
    char *question = with_current_time(text);
    llm_error_t llm_err = llm_stream(question ? question : text, on_llm_text, &view, &response);
    free(question);
    ESP_LOGI(TAG, "[timing] chat (%s, streamed): %lld ms", llm_provider_name(llm_provider()), elapsed_ms(t_start));

    if (llm_err == LLM_OK) {
        sentence_splitter_finish(&s_splitter);
    } else {
        // The error is shown and spoken as if it were the answer (replacing any partial text), as a local answer
        sentence_splitter_discard(&s_splitter);
        ui_ctrl_reply_set_logo(UI_CTRL_LOGO_LOCAL);
        reply_begin();
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

    bsp_spiffs_mount();
    // The languages offered (Languages of CONFIG.INI), then the texts of the language (LANG_<CODE>.TXT on SPIFFS)
    app_lang_init(sys_param->languages);
    app_lang_set(sys_param->lang);
    // Languages added to Languages of CONFIG.INI: their files, made from the English ones, then the configuration
    // mode to translate them (once the screen is up)
    char new_languages[96] = "";
    for (size_t i = 0; i < app_lang_count(); i++) {
        bool made = app_lang_create(app_lang_code(i), app_lang_name(i));
        made |= phrases_create(app_lang_code(i), app_lang_name(i));
        made |= persona_create(app_lang_code(i), app_lang_name(i));
        if (made) {
            snprintf(new_languages + strlen(new_languages), sizeof(new_languages) - strlen(new_languages), "%s%s",
                     new_languages[0] ? ", " : "", app_lang_name(i));
        }
    }
    // The countries known by voice (COUNTRY.TXT), a column for each language offered
    country_init();
    // The models offered (AGENTS.INI), then the free OpenRouter ones of the last list read (the live one is read
    // once a day, below): before the Settings screen lists them
    llm_load_models();
    openrouter_free_init();
    // The costs counted so far (prices of AGENTS.INI, in the currency of CONFIG.INI)
    costs_init(sys_param->currency);
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
    app_volume_init(sys_param->volume);

    ESP_LOGI(TAG, "Display LVGL demo");
    bsp_display_backlight_on();
    // Screen off when nobody is there (radar of the BOX-3-SENSOR dock, if present)
    app_presence_init(sys_param->screen_off_s);
    ui_ctrl_init();
    if (new_languages[0]) {
        char message[256];
        snprintf(message, sizeof(message), app_lang_get()->language_added, new_languages);
        ESP_LOGW(TAG, "%s", message);
        ui_ctrl_show_message(message);
        vTaskDelay(pdMS_TO_TICKS(NEW_LANGUAGE_MESSAGE_MS));
        settings_factory_reset();       // configuration mode: the files show up on the PC
    }
    app_network_start();

    ESP_LOGI(TAG, "speech recognition start");
    app_sr_start(false);

    // The Persona (name, who made it, who it talks to...) and the remembered notes complete the base instructions
    // Phrase files of the voice commands (installed at the first start), before the lists are made
    phrases_init();
    // Who the assistant is, per language (PERSO_FR.TXT...): the PERSONA.TXT of an older firmware moves there
    if (persona_init(app_lang_get()->code, sys_param->persona)) {
        settings_erase_key("Persona");
    }
    persona_load(app_lang_get()->code);
    app_memory_init(sys_param->memory);
    voice_cmd_init();
    weather_places_load();
    char *system_prompt = build_system_prompt();
    llm_init(sys_param->chat_model, sys_param->claude_key, sys_param->key, sys_param->url,
             sys_param->gemini_key, sys_param->openrouter_key,
             system_prompt ? system_prompt : app_lang_get()->system_prompt);
    free(system_prompt);
    llm_set_location(sys_param->country, sys_param->city);
    ESP_ERROR_CHECK(stt_stream_init());
    stt_stream_configure(sys_param->key, CONFIG_ASSISTANT_STT_MODEL, app_lang_get()->code);
    ESP_ERROR_CHECK(tts_stream_init());
    char *tts_instructions = build_tts_instructions();
    tts_stream_configure(sys_param->url, sys_param->key, CONFIG_ASSISTANT_TTS_MODEL, sys_param->voice,
                         tts_instructions ? tts_instructions : app_lang_get()->tts_instructions);
    free(tts_instructions);

    for (int seconds = 0; ; seconds++) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Homey's tools, read once the Wi-Fi is up (again every minute while the server cannot be reached)
        static int homey_retry_s = 0;
        if (sys_param->homey_url[0] && !homey_loaded && --homey_retry_s <= 0 &&
                wifi_connected_already() == WIFI_STATUS_CONNECTED_OK) {
            homey_mcp_set_confirm_off(sys_param->confirm_off);
            cJSON *homey_tools = homey_mcp_load_tools(sys_param->homey_url, sys_param->homey_key);
            if (homey_tools) {
                tools_add(homey_tools);
                homey_loaded = true;
            } else {
                homey_retry_s = 60;
            }
        }
        // "Édite la config": the conversation and the volume are saved, then configuration mode (USB drive)
        if (s_restart_requested) {
            // "Redémarre": the conversation and the volume are saved, the speaker switched off, then restart
            s_restart_requested = false;
            ESP_LOGI(TAG, "Restarting (voice command)");
            llm_history_save();
            app_volume_save();
            weather_places_save();
            costs_save();
            bsp_codec_mute_set(true);
            bsp_codec_power_amp_off();
            vTaskDelay(pdMS_TO_TICKS(30));
            esp_restart();
        }
        if (s_restore_requested) {
            // Icons of the Configuration screen: soft reset (then restart) or factory reset (then configuration
            // mode with a PC connected, otherwise restart)
            s_restore_requested = false;
            bool factory = s_factory_requested;
            ESP_LOGI(TAG, "%s reset", factory ? "Factory" : "Soft");
            bool done;
            if (factory) {
                done = (settings_factory_wipe() == ESP_OK);
            } else {
                llm_history_save();
                weather_places_save();
                costs_save();
                done = (settings_restore_defaults() == ESP_OK);
            }
            if (done) {
                remove_firmware_files(factory);
            }
            bsp_codec_mute_set(true);
            bsp_codec_power_amp_off();
            vTaskDelay(pdMS_TO_TICKS(30));
            if (factory && done && usb_serial_jtag_is_connected()) {
                settings_factory_reset();   // configuration mode: the drive shows up on the PC
            }
            esp_restart();
        }
        // "Teste les modèles": run here, on the main task's stack (several minutes, the loop waits meanwhile)
        model_test_poll();
        if (s_reset_requested) {
            s_reset_requested = false;
            llm_reset();
        }
        if (s_config_requested) {
            s_config_requested = false;
            ESP_LOGI(TAG, "Restarting in configuration mode");
            llm_history_save();
            app_volume_save();
            weather_places_save();
            costs_save();
            settings_factory_reset();
        }
        // Language or model changed from the Settings screen
        if (s_language_requested) {
            s_language_requested = false;
            assistant_set_language(s_requested_language);
        }
        if (s_model_requested) {
            s_model_requested = false;
            assistant_set_model(s_requested_model);
        }
        // The free OpenRouter models, read again once a day while asleep (a few seconds, ~800 KB)
        if (seconds % 60 == 30 && ui_ctrl_is_asleep() && !s_reply_active && !record_flag &&
                wifi_connected_already() == WIFI_STATUS_CONNECTED_OK && openrouter_free_refresh()) {
            ui_ctrl_models_changed();
        }
        // Conversation and volume saved once the assistant is back to sleep (sleep panel shown, no answer in
        // progress, not listening, nothing played for a while): never during an exchange, follow-up included.
        // The saves do nothing when unchanged. This task's stack is in internal RAM, as flash writes need.
        if (ui_ctrl_is_asleep() && !s_reply_active && !record_flag && tts_stream_is_silent(SAVE_AFTER_SILENCE_MS)) {
            llm_history_save();
            app_volume_save();
            weather_places_save();
            // The costs counted, saved (the conversation's stays shown until the next wake-up)
            costs_save();
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
