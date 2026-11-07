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
#include "esp_app_desc.h"
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
#include "file_lang.h"
#include "llm.h"
#include "file_memory.h"
#include "voice_cmd.h"
#include "debug_trace.h"
#include "file_state.h"
#include "google_calendar.h"
#include "file_phrases.h"
#include "ai_provider_openrouter_free.h"
#include "ai_providers.h"
#include "app_presence.h"
#include "assistant.h"
#include "app_volume.h"
#include "tools.h"
#include "weather.h"
#include "mcp_client.h"
#include "alarms.h"
#include "alarm_ring.h"
#include "sentence_splitter.h"
#include "tts_stream.h"
#include "stt_stream.h"
#include "cJSON.h"
#include "model_test.h"
#include "file_costs.h"
#include "file_persona.h"
#include "file_country.h"
#include "radio.h"
#include "radio_browser.h"
#include "file_radio_favorites.h"

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
/* Time to read what the reply shows but does not say (a list after "::"), in ms: the listen panel waits for it */
static uint32_t s_unread_ms = 0;
#define UNREAD_MAX_MS   20000
/* Characters of the reply shown (answer, or local message), for its reading time when it could not be spoken */
static size_t s_shown_chars = 0;
/* The reply itself is the "no OpenAI credit" message (not added again under it) */
static bool s_no_credit_shown = false;

/* Time to read these characters at the scrolling speed of the answers (Scroll_speed), at most UNREAD_MAX_MS */
static uint32_t reading_ms(size_t chars)
{
    uint32_t ms = chars * 1000 / (sys_param->scroll_cps > 0 ? sys_param->scroll_cps : 16);
    return ms > UNREAD_MAX_MS ? UNREAD_MAX_MS : ms;
}
/*
 * Flash writes pause the tasks running from flash (microphone feed included): the conversation is saved once
 * nothing has been heard or played for this long, i.e. after the follow-up listening, not during it
 */
#define SAVE_AFTER_SILENCE_MS           300
/* A language added to Languages: its message stays this long before the configuration mode */
#define NEW_LANGUAGE_MESSAGE_MS         6000
/* The instructions given have the home rules (a Homey switched on) */
static bool s_prompt_homey = false;
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
/* Double tap while listening or thinking: what is under way is dropped (assistant_cancel) */
static volatile bool s_cancelled = false;
/* A radio command was answered: back to sleep after the reply (the radio plays then), without listening again */
static volatile bool s_sleep_after_local = false;
/* The reply is the one of a voice command handled here (not of the model): Sleep_after_cmd applies */
static volatile bool s_reply_local_cmd = false;
/* The reply ended with a goodbye ("Au revoir !", "Bonne soirée."): sleep and a new conversation once spoken */
static volatile bool s_reply_goodbye = false;
static volatile bool s_reset_requested = false;
/* Dump button of the Debug page: the traces written, then the configuration mode (with a PC) */
static volatile bool s_dump_requested = false;
/* The main task, woken up early to save as soon as the assistant sleeps */
static TaskHandle_t s_main_task = NULL;
/* The gender of the voice the instructions were made with; they are given to the chat client once it is set up */
static char s_prompt_gender = 0;
static bool s_prompt_ready = false;

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
static char *speech_refused_text(llm_speech_kind_t kind);

static int64_t s_first_text_us = 0;
static int64_t elapsed_ms(int64_t start_us);

static void on_llm_text(const char *delta, void *ctx)
{
    reply_view_t *v = (reply_view_t *)ctx;
    if (s_cancelled) {
        return;     // dropped: nothing shown nor spoken
    }
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
    if (!s_first_text_us) {
        s_first_text_us = esp_timer_get_time();
        TRACE(TAG, "first text of the model %lld ms after the transcript", elapsed_ms(s_reply_t0));
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
    bool digit = false;
    for (const char *p = text; *p; p++) {
        if (isalnum((unsigned char)*p) || ((unsigned char)*p & 0x80)) {
            letters++;
        }
        digit |= isdigit((unsigned char)*p) != 0;
    }
    // A lone digit is kept: the answer to a numbered list ("3.")
    if (letters < 2 && !digit) {
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
    TRACE(TAG, "sentence to the speech %lld ms after the transcript: \"%.60s%s\"", elapsed_ms(s_reply_t0), sentence,
          strlen(sentence) > 60 ? "..." : "");
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
    size_t chars = s_shown_chars;
    if (tts_stream_no_credit()) {
        // Nothing spoken: the speech service refused (no credit, plan too small), said on screen (it cannot be
        // spoken)
        if (!s_no_credit_shown) {
            char *refused = speech_refused_text(LLM_SPEECH_TTS);
            ui_ctrl_reply_append_notice(refused ? refused : "");
            chars += refused ? strlen(refused) : 0;
            free(refused);
        }
    } else if (tts_stream_reply_was_choppy()) {
        // Speech cut or sentences lost: the answer stays readable on screen, say why it was heard badly
        ui_ctrl_reply_append_notice(app_lang_get()->voice_choppy);
        chars += strlen(app_lang_get()->voice_choppy);
    }
    if (!ui_ctrl_reply_get_audio_start_flag()) {
        // No audio at all (refused, network down): the text scrolls from now at the reading speed (Scroll_speed),
        // the whole reply is to be read before the listen panel comes
        s_unread_ms = reading_ms(chars);
        ui_ctrl_reply_set_audio_start_flag(true);
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
    if (s_cancelled) {
        // Dropped by a double tap (a question of a voice command among others): asleep already, no listening
        return;
    }
    if (record_flag) {
        // Reply interrupted by a new listening session (wake word or touch)
        return;
    }
    if (s_reply_local_cmd && (sys_param->sleep_after_cmd || s_sleep_after_local) && !voice_cmd_waiting_answer()) {
        // Sleep_after_cmd, or a radio command: a voice command handled here (and asking nothing) does not listen again
        s_sleep_after_local = false;
        ESP_LOGI(TAG, "Voice command done, going to sleep");
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
        // A list shown but not read (a question asked about it): the reply stays shown while listening, the time to
        // read it added to the window
        if (s_unread_ms > 0) {
            ESP_LOGI(TAG, "Listening, the reply stays shown (%u ms to read it)", (unsigned)s_unread_ms);
        }
        app_sr_start_followup(CONFIG_ASSISTANT_FOLLOWUP_WINDOW_MS + s_unread_ms, s_unread_ms > 0);
        return;
    }
#endif
    // Back to sleep once the answer has scrolled (no follow-up, or a volume change)
    ui_ctrl_reply_set_audio_end_flag(true);
}

/*
 * Woken up after a pause of Resume_ask_min or more in a conversation (the reasoning mode or not; after a restart too,
 * its time kept with it): "Veux-tu continuer la conversation précédente ?" is asked instead of listening at once (its
 * answer is listened to after it, voice_cmd.c). Called from app_audio.c (SR handler task); false: listen as usual.
 */
bool assistant_resume_ask(void)
{
    int64_t last_us = llm_last_exchange_us();
    int64_t pause_us = esp_timer_get_time() - last_us;
    if (sys_param->resume_ask_min <= 0 || !last_us || pause_us < sys_param->resume_ask_min * 60 * 1000000LL) {
        return false;
    }
    ESP_LOGI(TAG, "%s, %lld min since the last answer: going on?", llm_reasoning() ? "Reasoning mode" : "Conversation",
             pause_us / 60000000LL);
    // Asked once for this pause: the answer (or a follow-up) does not ask it again
    llm_exchange_touch();
    s_cancelled = false;
    llm_cancel_clear();
    voice_cmd_expect_resume();
    speak_message(app_lang_get()->resume_ask);
    return true;
}

/*
 * Woken up while a question of a voice command (model, language, town...) is still unanswered (the assistant went
 * to sleep without its answer): it is asked again, its answer listened to after it ("abandon" drops it). Called
 * from app_audio.c (SR handler task); false: listen as usual.
 */
bool assistant_question_ask(void)
{
    const char *question = voice_cmd_question();
    if (!question) {
        return false;
    }
    ESP_LOGI(TAG, "Woken up with a question unanswered: asked again");
    s_cancelled = false;
    llm_cancel_clear();
    char *text = strdup(question);
    if (!text) {
        return false;
    }
    speak_message(text);
    free(text);
    return true;
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
    s_first_text_us = 0;
    s_reply_active = true;
    s_reply_local_cmd = false;
    s_reply_goodbye = false;
    s_unread_ms = 0;
    s_shown_chars = 0;
    s_no_credit_shown = false;
    tts_stream_begin(on_reply_audio);
}

/*
 * A "::" or ".." of the LANG texts comes before a list ("Quel fournisseur voulez-vous :: 1, Anthropic. 2, OpenAI."):
 * shown as ":" or ".", and with Read_lists = off the list after it is not read, up to the end of the reply or
 * VOICE_CMD_LIST_END (a question follows); a part between two VOICE_CMD_SHOWN_ONLY is never read. "..." stays an
 * ellipsis. shown and spoken: as long as the message. true: a list is not read
 */
static bool list_reply_texts(const char *message, char *shown, char *spoken)
{
    bool silent = false;
    bool list_unread = false;
    for (const char *p = message; *p; p++) {
        bool colons = (p[0] == ':' && p[1] == ':');
        bool dots = (p[0] == '.' && p[1] == '.' && p[2] != '.' && (p == message || p[-1] != '.'));
        if (colons || dots) {
            *shown++ = *p;
            if (!silent) {
                *spoken++ = *p;
            }
            silent = !sys_param->read_lists;
            list_unread |= silent;
            p++;
        } else if (*p == VOICE_CMD_LIST_END[0]) {
            silent = false;
        } else if (*p == VOICE_CMD_SHOWN_ONLY[0]) {
            // Up to the next one: shown, never spoken
            for (p++; *p && *p != VOICE_CMD_SHOWN_ONLY[0]; p++) {
                *shown++ = *p;
            }
            if (!*p) {
                break;
            }
        } else {
            *shown++ = *p;
            if (!silent) {
                *spoken++ = *p;
            }
        }
    }
    *shown = '\0';
    *spoken = '\0';
    return list_unread;
}

/* Show and speak a message as the reply (errors, memory commands), sentence by sentence */
static void speak_message(const char *message)
{
    size_t size = strlen(message) + 1;
    char *shown = malloc(size);
    char *spoken = malloc(size);
    bool list_unread = false;
    if (shown && spoken) {
        list_unread = list_reply_texts(message, shown, spoken);
    }
    ui_ctrl_reply_set_logo(UI_CTRL_LOGO_LOCAL);
    ui_ctrl_label_show_text(UI_CTRL_LABEL_REPLY_CONTENT, shown && spoken ? shown : message);
    ui_ctrl_show_panel(UI_CTRL_PANEL_REPLY, 0);
    if (list_unread) {
        // Scrolled with the finger, from its top
        ui_ctrl_reply_list_shown();
    }
    s_reply_t0 = esp_timer_get_time();
    reply_begin();
    if (shown && spoken) {
        // The characters not said, read at the scrolling speed of the answers (Scroll_speed)
        size_t unread = strlen(shown) - strlen(spoken);
        s_unread_ms = reading_ms(unread);
        s_shown_chars = strlen(shown);
    }
    sentence_splitter_init(&s_splitter, on_sentence, NULL);
    sentence_splitter_feed(&s_splitter, shown && spoken ? spoken : message);
    sentence_splitter_finish(&s_splitter);
    tts_stream_end();
    free(shown);
    free(spoken);
}

/*
 * Instructions given to the model: the base ones, who the assistant is (PERSO_<XX>.TXT) and the notes the user asked
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
    s_prompt_homey = mcp_homey_on();
    const char *home = s_prompt_homey ? lang->home_line : "";
    const char *home_rules = s_prompt_homey ? lang->home_rules : "";
    // Identity, personality and instructions of PERSO_<LANG>.TXT, then what the working requires (file_lang.c),
    // the location, the home and the notes to remember
    // Speaking of itself as the voice it has: a woman's or a man's (French, Spanish... agree with it)
    const llm_voice_t *voice = assistant_voice();
    const char *gender = !voice ? "" : voice->gender == 'f' ? lang->voice_female :
                         voice->gender == 'm' ? lang->voice_male : "";
    ESP_LOGI(TAG, "Instructions made: voice %s, %s", voice ? voice->name : "-",
             !voice || !voice->gender ? "no gender said" : voice->gender == 'f' ? "a woman's" : "a man's");
    const char *parts[] = {
        persona_get(PERSONA_IDENTITY), persona_get(PERSONA_PERSONALITY), persona_get(PERSONA_INSTRUCTIONS), gender,
        lang->system_prompt, location, home, home_rules,
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

/* Instructions of the speech synthesis: read word for word (file_lang.c), then the [voice] of PERSO_<LANG>.TXT */
/* Reading instructions (OpenAI): read verbatim, the voice of the persona, the emotion and pace of the voice */
static char *build_tts_instructions(const llm_voice_t *v)
{
    const app_lang_t *lang = app_lang_get();
    char *text = NULL;
    const char *voice = persona_get(PERSONA_VOICE);
    char emotion[96] = "", pace[96] = "";
    if (v && v->emotion) {
        snprintf(emotion, sizeof(emotion), lang->tts_emotion, v->emotion);
    }
    if (v && v->pace) {
        snprintf(pace, sizeof(pace), lang->tts_pace, v->pace);
    }
    asprintf(&text, "%s%s%s%s%s%s%s", lang->tts_instructions, voice[0] ? " " : "", voice, emotion[0] ? " " : "",
             emotion, pace[0] ? " " : "", pace);
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

/* Files written by the assistant, removed by the factory reset (sounds of the image kept) */
static const char *const s_user_files[] = {
    "history.json", "history.tmp", "places.json", "or_free.json", "record.wav", "model_times.json", "speed.txt", "log.txt",
    "costs.json", "costs.tmp", "costs.txt", "radio.txt", "radio.txt.tmp",
};

/* Languages whose files are given with the firmware: a soft reset restores them; those of a language added by the
 * user (its translation) are kept */
static const char *const s_firmware_languages[] = { "fr", "en", "nl", "de", "es", "it" };

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
        // The conversation, the costs, the test results and the places (LittleFS)
        state_erase();
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

/* Texts of the language in use missing (added in English) or with wrong %s: a screen says which ("Skip", "Config") */
static void show_lang_problems(void)
{
    char *problems = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    if (problems && app_lang_problems(problems, 1024)) {
        char title[24];
        snprintf(title, sizeof(title), "LANG_%s.TXT", app_lang_get()->code);
        for (char *c = title + 5; *c && *c != '.'; c++) {
            *c = toupper((unsigned char)*c);
        }
        ESP_LOGW(TAG, "%s", problems);
        ui_ctrl_show_problems(title, problems);
    }
    free(problems);
}

void assistant_request_config(void)
{
    s_config_requested = true;      // the main task saves and restarts in configuration mode
}

void assistant_save_soon(void)
{
    if (s_main_task) {
        xTaskNotifyGive(s_main_task);
    }
}

void assistant_request_dump(void)
{
    s_dump_requested = true;        // done by the main task
}

void assistant_set_language(const char *code)
{
    strlcpy(sys_param->lang, code, sizeof(sys_param->lang));
    app_lang_set(code);
    const app_lang_t *lang = app_lang_get();
    // Who the assistant is in this language (PERSO_<LANG>.TXT)
    persona_load(lang->code);
    stt_stream_set_language(lang->code);
    // The reading instructions of the language, and a voice that speaks it
    assistant_set_tts(NULL);
    char *prompt = build_system_prompt();
    if (prompt) {
        llm_set_system_prompt(prompt);
        free(prompt);
    }
    ui_ctrl_apply_language();
    ESP_LOGI(TAG, "Language %s applied", code);
    // Its texts checked as at start-up: missing ones (added in English) or with wrong %s, said on screen
    show_lang_problems();
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
    llm_init(model, sys_param->url, prompt ? prompt : app_lang_get()->system_prompt);
    s_prompt_ready = true;
    free(prompt);
    llm_set_location(sys_param->country, sys_param->city);
    ui_ctrl_settings_sync();
    ESP_LOGI(TAG, "Model %s applied", model);
}

/* The provider of a speech model ([stt] / [tts] of AGENTS.INI), OpenAI's when unknown */
static const ai_provider_t *speech_provider(const char *provider)
{
    const ai_provider_t *p = ai_provider_find(provider);
    return p ? p : ai_provider_of_llm(LLM_PROVIDER_OPENAI);
}

const char *assistant_speech_key(const char *provider)
{
    return ai_provider_key(speech_provider(provider));
}

const char *assistant_speech_key_name(const char *provider)
{
    return speech_provider(provider)->key_name;
}

const char *assistant_speech_company(const char *provider)
{
    return speech_provider(provider)->company;
}

const struct stt_backend *assistant_stt_backend(const char *provider)
{
    const ai_provider_t *p = speech_provider(provider);
    return p->stt ? p->stt : &stt_backend_openai;
}

const struct tts_backend *assistant_tts_backend(const char *provider)
{
    const ai_provider_t *p = speech_provider(provider);
    return p->tts ? p->tts : &tts_backend_openai;
}

/*
 * The transcription / speech model of CONFIG.INI with its provider ([stt] / [tts] of AGENTS.INI) and the key of
 * that provider; one whose provider has no key (Mistral_key, Deepgram_key) falls back to OpenAI's default model
 * (nothing would work)
 */
static const llm_speech_model_t *speech_choice(llm_speech_kind_t kind, const char *model, const char **key)
{
    const llm_speech_model_t *m = llm_speech_find(kind, model);
    if (m && !assistant_speech_key(m->provider)[0]) {
        ESP_LOGW(TAG, "%s: no %s, OpenAI's model is used", model, assistant_speech_key_name(m->provider));
        m = llm_speech_find(kind, kind == LLM_SPEECH_STT ? CONFIG_ASSISTANT_STT_MODEL : CONFIG_ASSISTANT_TTS_MODEL);
    }
    *key = assistant_speech_key(m ? m->provider : "openai");
    return m;
}

/* "(ElevenLabs refuse la voix...)": the service of the transcription or the speech in use named (allocated) */
static char *speech_refused_text(llm_speech_kind_t kind)
{
    const char *key;
    const llm_speech_model_t *m = speech_choice(kind, kind == LLM_SPEECH_STT ? sys_param->stt_model :
                                                sys_param->tts_model, &key);
    char *text = NULL;
    asprintf(&text, app_lang_get()->speech_refused, assistant_speech_company(m ? m->provider : "openai"));
    return text;
}

const char *assistant_tts_provider(void)
{
    const char *key;
    const llm_speech_model_t *m = speech_choice(LLM_SPEECH_TTS, sys_param->tts_model, &key);
    return llm_speech_voice_owner(m);     // a model behind OpenRouter: its own voices
}

const struct llm_voice *assistant_voice(void)
{
    return llm_voice_for(assistant_tts_provider(), app_lang_get()->code, sys_param->voice);
}

void assistant_set_stt(const char *model)
{
    if (model) {
        strlcpy(sys_param->stt_model, model, sizeof(sys_param->stt_model));
    }
    const char *key;
    const llm_speech_model_t *m = speech_choice(LLM_SPEECH_STT, sys_param->stt_model, &key);
    stt_stream_configure(assistant_stt_backend(m ? m->provider : "openai"), key,
                         m ? m->id : CONFIG_ASSISTANT_STT_MODEL, app_lang_get()->code);
}

void assistant_set_tts(const char *model)
{
    if (model) {
        strlcpy(sys_param->tts_model, model, sizeof(sys_param->tts_model));
    }
    const char *key;
    const llm_speech_model_t *m = speech_choice(LLM_SPEECH_TTS, sys_param->tts_model, &key);
    bool mistral = m && strcmp(m->provider, "mistral") == 0;
    const llm_voice_t *v = llm_voice_for(llm_speech_voice_owner(m), app_lang_get()->code, sys_param->voice);
    char *tts_instructions = build_tts_instructions(v);
    // Mistral (Voxtral 3): the emotion and pace as tags before each sentence; OpenAI: in its instructions
    char style[64] = "";
    if (mistral && v && v->emotion) {
        snprintf(style, sizeof(style), "[emotion=%s]", v->emotion);
    }
    if (mistral && v && v->pace) {
        snprintf(style + strlen(style), sizeof(style) - strlen(style), "[pace=%s]", v->pace);
    }
    if (style[0]) {
        strlcat(style, " ", sizeof(style));
    }
    tts_stream_configure(assistant_tts_backend(m ? m->provider : "openai"), sys_param->url, key,
                         m ? m->id : CONFIG_ASSISTANT_TTS_MODEL, v ? v->id : NULL,
                         tts_instructions ? tts_instructions : app_lang_get()->tts_instructions,
                         style[0] ? style : NULL);
    free(tts_instructions);
    // Another voice of the other gender: the instructions say it (once the chat client is set up), and the
    // conversation starts afresh, as after "nouvelle discussion" (its answers in the other gender won over them)
    char gender = v ? v->gender : 0;
    if (s_prompt_ready && gender != s_prompt_gender) {
        char *prompt = build_system_prompt();
        if (prompt) {
            llm_set_system_prompt(prompt);
            free(prompt);
            ESP_LOGI(TAG, "Voice %s: the instructions say it is %s, new conversation", v ? v->name : "-",
                     gender == 'f' ? "a woman's" : gender == 'm' ? "a man's" : "neither a woman's nor a man's");
        }
        s_reset_requested = true;       // the main task clears the conversation
    }
    s_prompt_gender = gender;
}

/* Start of a listening session (wake word, touch or follow-up). Called from app_audio.c */
/* Double tap while "J'écoute" / "Je réfléchis": what is under way is dropped, the assistant goes back to sleep */
void assistant_cancel(void)
{
    ESP_LOGI(TAG, "Cancelled by a double tap");
    s_cancelled = true;
    voice_cmd_cancel_question();    // a voice command's question (language, model...): its answer no longer awaited
    app_sr_cancel();        // listening: ended as without speech
    llm_cancel();           // thinking: the answer's stream is left
    tts_stream_cancel();
    ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, 0);
}

void assistant_listen_start(void)
{
    s_cancelled = false;
    llm_cancel_clear();
    // The transcript is not shown while the user speaks: the question appears above the answer
    stt_stream_start(NULL);
}

/* Open the chat connection while the user speaks (speech uses a permanent WebSocket). Called from app_audio.c */
void assistant_prewarm(void)
{
    /* The transcription connection is the most urgent: let its TLS handshake finish first */
    stt_stream_wait_connected(3000);
    tts_stream_prewarm();   // done by the synthesis task, meanwhile
    llm_prewarm();
}

/* Nobody spoke: drop the transcription session. Called from app_audio.c */
void assistant_listen_abort(void)
{
    stt_stream_abort();
}

/* The answer given by a tap on an item of a list ("3"), used instead of the transcript */
static char *s_tapped_text = NULL;

/* End of speech: get the final transcript, then answer. Called from app_audio.c (SR handler task) */
void assistant_listen_end(void)
{
    esp_err_t ret = ESP_OK;
    char *response = NULL;
    int64_t t_start;

    ui_ctrl_show_panel(UI_CTRL_PANEL_GET, 0);

    t_start = esp_timer_get_time();
    char *text = s_tapped_text ? s_tapped_text : stt_stream_finish(STT_FINAL_TIMEOUT_MS);
    s_tapped_text = NULL;
    ESP_LOGI(TAG, "[timing] final transcript %lld ms after the end of speech", elapsed_ms(t_start));
    if (s_cancelled) {
        ESP_LOGI(TAG, "Question dropped (cancelled)");
        free(text);
        return;
    }
    if (NULL == text) {
        // Not understood, or the transcription service refused (no credit...): shown, it cannot be spoken either
        char *refused = stt_stream_no_credit() ? speech_refused_text(LLM_SPEECH_STT) : NULL;
        speak_message(refused ? refused : app_lang_get()->stt_error);
        free(refused);
        s_no_credit_shown = stt_stream_no_credit();
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
        voice_cmd_cancel_question();    // not asked again on waking up
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
        s_sleep_after_local = (voice_cmd == VOICE_CMD_RADIO);
        speak_message(cmd_reply ? cmd_reply : "");
        // A reply asking to go on ("Je t'écoute."): listened to again, whatever Sleep_after_cmd
        s_reply_local_cmd = (voice_cmd != VOICE_CMD_LISTEN);
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
                           llm_provider() == LLM_PROVIDER_OPENROUTER ? UI_CTRL_LOGO_OPENROUTER :
                           llm_provider() == LLM_PROVIDER_XAI ? UI_CTRL_LOGO_XAI :
                           llm_provider() == LLM_PROVIDER_MISTRAL ? UI_CTRL_LOGO_MISTRAL :
                           llm_provider() == LLM_PROVIDER_PERPLEXITY ? UI_CTRL_LOGO_PERPLEXITY : UI_CTRL_LOGO_CLAUDE);
    char *question = with_current_time(text);
    llm_error_t llm_err = s_cancelled ? LLM_ERR_OTHER : llm_stream(question ? question : text, on_llm_text, &view,
                                                                  &response);
    free(question);
    ESP_LOGI(TAG, "[timing] chat (%s, streamed): %lld ms", llm_provider_name(llm_provider()), elapsed_ms(t_start));
    if (s_cancelled) {
        // Double tap while thinking: nothing shown nor spoken (the assistant already sleeps)
        ESP_LOGI(TAG, "Answer dropped (cancelled)");
        sentence_splitter_discard(&s_splitter);
        tts_stream_cancel();
        free(view.text);
        goto err;
    }

    if (llm_err == LLM_OK) {
        sentence_splitter_finish(&s_splitter);
    } else {
        // The error is shown and spoken as if it were the answer (replacing any partial text), as a local answer
        sentence_splitter_discard(&s_splitter);
        ui_ctrl_reply_set_logo(UI_CTRL_LOGO_LOCAL);
        reply_begin();
        char *effort_message = NULL;
        if (llm_err == LLM_ERR_EFFORT) {
            // "Le modèle Mistral Medium 3.5 refuse le niveau de raisonnement low. Dans AGENTS.INI, mettez : high, none."
            const char *model = sys_param->chat_model;
            for (size_t i = 0; i < llm_model_count; i++) {
                if (strcmp(llm_models[i].id, sys_param->chat_model) == 0) {
                    model = llm_models[i].label;
                }
            }
            asprintf(&effort_message, app_lang_get()->error_effort, model, llm_effort_refused, llm_effort_values);
        }
        const char *message = effort_message ? effort_message :
                              app_lang_get()->llm_errors[llm_provider()][llm_err == LLM_ERR_EFFORT ? LLM_ERR_OTHER : llm_err];
        tts_stream_say(message);
        free(response);
        response = strdup(message);
        free(effort_message);
        if (view.text) {
            view.text[prefix_len] = '\0';
            view.len = prefix_len;
        }
        reply_view_append(&view, response ? response : "");
        ESP_LOGW(TAG, "Chat error %d: speaking \"%s\"", llm_err, response ? response : "");
    }
    reply_view_refresh(&view);
    s_shown_chars = view.len;
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

/*
 * An item of the list shown was tapped: its number is the answer, as if said (the listening going on, if any, is
 * dropped). Called from app_audio.c (SR handler task)
 */
void assistant_answer_tapped(int number, bool was_listening)
{
    ESP_LOGI(TAG, "Item %d of the list tapped: taken as the answer", number);
    if (was_listening) {
        stt_stream_abort();
    }
    s_cancelled = false;
    llm_cancel_clear();
    voice_cmd_reply_spoken();       // answered now, however long the list was read
    if (asprintf(&s_tapped_text, "%d", number) < 0) {
        s_tapped_text = NULL;
        return;
    }
    assistant_listen_end();
}



static void *psram_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* Why the device started, as said in the traces and BOOTS.TXT */
static const char *reset_reason_text(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:   return "power on";
    case ESP_RST_EXT:       return "external reset";
    case ESP_RST_SW:        return "restart by the firmware (asked, update, configuration mode)";
    case ESP_RST_PANIC:     return "CRASH (panic: exception or abort)";
    case ESP_RST_INT_WDT:   return "CRASH (interrupt watchdog)";
    case ESP_RST_TASK_WDT:  return "CRASH (task watchdog)";
    case ESP_RST_WDT:       return "CRASH (other watchdog)";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    case ESP_RST_BROWNOUT:  return "POWER DROP (brownout)";
    case ESP_RST_SDIO:      return "reset by SDIO";
    case ESP_RST_USB:       return "reset by USB (flash, Reset button)";
    case ESP_RST_JTAG:      return "reset by JTAG";
    default:                return "unknown";
    }
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
    // The time zone at once (the clock of the previous run goes on): the times of the traces are local from the start
    setenv("TZ", CONFIG_ASSISTANT_TIMEZONE, 1);
    tzset();
    // Debug traces: those of the run before kept unless the power was cut, then every log line kept (DEBUG.TXT)
    esp_reset_reason_t reset = esp_reset_reason();
    debug_trace_boot(reset != ESP_RST_POWERON && reset != ESP_RST_BROWNOUT);
    debug_trace_set(sys_param->debug);
    ESP_LOGW(TAG, "========== START: %s (firmware %s) ==========", reset_reason_text(reset),
             esp_app_get_description()->version);

    bsp_spiffs_mount();
    // The files written often (conversation, costs, test results, places): LittleFS, moved there from SPIFFS once
    state_mount();
    // This start in the history (BOOTS.TXT of the configuration drive)
    state_boot_record(esp_app_get_description()->version, reset_reason_text(reset));
    // Google Calendar: the account linked (its token, on LittleFS)
    gcal_init();
    // The MCP servers of CONFIG.INI (Homey...): their tools read once the Wi-Fi is up
    mcp_init();
    // Timers, alarms and reminders kept (LittleFS)
    alarms_init();
    // The backup of CONFIG.INI made by the factory reset of an older firmware (keys included): no longer kept
    if (remove("/spiffs/config.bkp") == 0) {
        ESP_LOGI(TAG, "CONFIG.BKP removed");
    }
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
    // The favourite radios (RADIO.TXT), made with its header when missing
    radio_favorites_init();
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
    // Texts of the language in use missing (added in English) or with wrong %s: said on screen
    show_lang_problems();

    ESP_LOGI(TAG, "speech recognition start");
    app_sr_start(false);

    // Who the assistant is (PERSO_<XX>.TXT) and the remembered notes complete the base instructions
    // Phrase files of the voice commands (installed at the first start), before the lists are made
    phrases_init();
    // Who the assistant is, per language (PERSO_FR.TXT...)
    persona_init();
    persona_load(app_lang_get()->code);
    app_memory_init(sys_param->memory);
    voice_cmd_init();
    weather_places_load();
    char *system_prompt = build_system_prompt();
    llm_init(sys_param->chat_model, sys_param->url, system_prompt ? system_prompt : app_lang_get()->system_prompt);
    free(system_prompt);
    llm_set_location(sys_param->country, sys_param->city);
    // Transcription and speech: the models of CONFIG.INI (Stt_model, Tts_model), by their provider
    ESP_ERROR_CHECK(stt_stream_init());
    assistant_set_stt(NULL);
    ESP_ERROR_CHECK(tts_stream_init());
    assistant_set_tts(NULL);
    // The screens were made before: the provider's icon on the robot's forehead, the Settings screen
    ui_ctrl_settings_sync();

    // Ringing the timers, alarms and reminders due (its chime made once)
    alarm_ring_init();

    s_main_task = xTaskGetCurrentTaskHandle();
    for (int seconds = 0; ; seconds++) {
        // A second, or less when the assistant goes to sleep with something to save (assistant_save_soon)
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        // A timer, an alarm or a reminder due: rung once the assistant sleeps (after a reply, a conversation)
        if (!alarm_ring_active() && !s_reply_active && ui_ctrl_is_asleep()) {
            alarm_t due;
            bool missed = false;
            if (alarms_due(&due, &missed)) {
                alarm_ring_start(&due, missed);
            }
        }
        // The tools of the MCP servers (Homey...), read once the Wi-Fi is up (again every minute while one cannot be
        // reached), or switched on or off on the MCP page: the model's tools, the home rules, the page
        if (mcp_poll(wifi_connected_already() == WIFI_STATUS_CONNECTED_OK)) {
            if (mcp_homey_on() != s_prompt_homey) {
                char *prompt = build_system_prompt();
                if (prompt) {
                    llm_set_system_prompt(prompt);
                    free(prompt);
                }
            }
            ui_ctrl_mcp_changed();
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
        // The models enabled or disabled on the Settings screen (their boxes drawn already)
        ui_ctrl_models_apply();
        // Google Calendar being linked (its code asked, then whether the user accepted), or unlinked
        gcal_poll();
        if (s_reset_requested) {
            s_reset_requested = false;
            llm_reset();
        }
        if (s_dump_requested) {
            s_dump_requested = false;
            if (usb_serial_jtag_is_connected()) {
                s_config_requested = true;      // dumped by settings_factory_reset()
            } else {
                // No PC to show the drive: written anyway, DEBUG.TXT of the next configuration mode
                debug_trace_dump();
                ui_ctrl_toast(app_lang_get()->config_need_pc);
            }
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
            // (a fraction of a second on LittleFS, file_state.h)
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
