/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Games between two BOX: see game.h.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "settings.h"
#include "file_lang.h"
#include "file_persona.h"
#include "llm.h"
#include "tts_stream.h"
#include "radio.h"
#include "app_wifi.h"
#include "app_ui_ctrl.h"
#include "game.h"

static const char *TAG = "game";

#define GAMES_FILE          "/spiffs/games.ini"
#define GAMES_MAX           12
#define GAME_TEXT_MAX       1200        /* a line, at most (UDP) */
/* Looking for the other BOX: answered within this, else "no other BOX" */
#define GAME_INVITE_MS      3000
/* The other BOX silent this long: the game ends */
#define GAME_SILENCE_MS     90000
/* Each message sent this many times (UDP on a local network: rarely lost; duplicates are dropped by the turn) */
#define GAME_SEND_TIMES     2
#define GAME_TURNS_DEFAULT  20

/* main.c */
extern void assistant_show(const char *text);
extern void assistant_say(const char *text);
extern void assistant_save_soon(void);
extern void assistant_reply_stop(void);

/* The firmware's games, installed when missing (main/games/games.ini, embedded NUL-terminated) */
extern const char s_games_default_start[] asm("_binary_games_ini_start");

/*
 * Personalities drawn for Game_tone = random (in English: the model speaks the language of the BOX). Two BOX never
 * get the same one: the one starting says which it drew.
 */
static const char *const s_tones[] = {
    "a pompous philosopher who quotes thinkers who never existed",
    "a blase teenager who finds everything 'too much'",
    "a cheerful pirate who speaks like one",
    "an absent-minded professor who gets lost in details",
    "a dramatic poet who turns everything into tragedy",
    "a passionate chef who relates everything to cooking",
    "an over-excited sports commentator",
    "a grumpy old sailor who has seen it all",
    "an extremely polite butler",
    "a medieval knight, noble and a little outdated",
    "a science-fiction fan convinced robots will rule the world",
    "a cheerful grandmother full of proverbs",
};
#define TONES_COUNT (sizeof(s_tones) / sizeof(s_tones[0]))

typedef struct {
    char id[24];
    char name[64];
    char phrases[400];      /* of the current language, "a | b | c" */
    char *first;
    char *rules;
} game_def_t;

static game_def_t *s_games = NULL;      /* PSRAM */
static int s_game_count = 0;
static char s_games_lang[8] = "";

/* A message received, given by the network task to the main task */
typedef struct {
    char type[8];           /* invite, join, line, done, stop, end */
    uint64_t from;          /* the sender's id (its MAC) */
    uint32_t ip;            /* its address (network order) */
    uint32_t nonce;         /* the game's */
    int turn;
    int tone;               /* the tone drawn by the one inviting / joining, -1 for none */
    char game[24];
    char *text;             /* a line, allocated */
} game_msg_t;

typedef enum {
    G_IDLE,
    G_INVITING,             /* looking for the other BOX */
    G_PLAYING,              /* lines taken in turn */
} game_state_t;

static int s_sock = -1;
static QueueHandle_t s_queue = NULL;
static uint64_t s_my_id = 0;

static volatile game_state_t s_state = G_IDLE;
static volatile bool s_stop_requested = false;
static int s_game = -1;
static uint32_t s_nonce = 0;
static uint64_t s_peer_id = 0;
static uint32_t s_peer_ip = 0;
static bool s_starter = false;
static int s_turns = 0;             /* lines said so far (both) */
static int s_turns_max = GAME_TURNS_DEFAULT;
static int s_peer_done = 0;         /* the last line the other BOX finished speaking */
static int s_my_tone = -1;
static char *s_tone_text = NULL;    /* the personality given to the model */
static cJSON *s_history = NULL;     /* [{"me":true,"text":"..."}] */
static char *s_ready_line = NULL;   /* my next line, made while the other speaks */
static int64_t s_since = 0;         /* the start of the current wait */

/* ---------- games.ini ---------- */

static void trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
}

/* The firmware's file installed when missing or emptied */
static void games_install(void)
{
    FILE *f = fopen(GAMES_FILE, "r");
    long size = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fclose(f);
    }
    if (size > 0) {
        return;
    }
    f = fopen(GAMES_FILE, "w");
    bool ok = f && fputs(s_games_default_start, f) >= 0;
    if (f) {
        ok &= fclose(f) == 0;
    }
    ESP_LOGI(TAG, "%s %s", ok ? "Installed" : "Could not install", GAMES_FILE);
}

static void games_free(void)
{
    for (int i = 0; i < s_game_count; i++) {
        free(s_games[i].first);
        free(s_games[i].rules);
    }
    s_game_count = 0;
}

/* Read again for the language in use (its names and phrases, else the English ones) */
static void games_load(void)
{
    const char *lang = app_lang_get()->code;
    if (s_games && strcmp(s_games_lang, lang) == 0) {
        return;
    }
    if (!s_games) {
        s_games = heap_caps_calloc(GAMES_MAX, sizeof(game_def_t), MALLOC_CAP_SPIRAM);
    }
    games_free();
    strlcpy(s_games_lang, lang, sizeof(s_games_lang));
    FILE *f = s_games ? fopen(GAMES_FILE, "r") : NULL;
    if (!f) {
        return;
    }
    char key_name[24], key_phrases[24];
    snprintf(key_name, sizeof(key_name), "name_%s", lang);
    snprintf(key_phrases, sizeof(key_phrases), "phrases_%s", lang);
    for (char *c = key_name; *c; c++) {
        *c = (char)tolower((unsigned char)*c);
    }
    for (char *c = key_phrases; *c; c++) {
        *c = (char)tolower((unsigned char)*c);
    }
    char *line = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    game_def_t *g = NULL;
    bool name_own = false, phrases_own = false;
    while (line && fgets(line, 2048, f)) {
        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (!end || s_game_count >= GAMES_MAX) {
                g = NULL;
                continue;
            }
            *end = '\0';
            g = &s_games[s_game_count++];
            memset(g, 0, sizeof(*g));
            strlcpy(g->id, line + 1, sizeof(g->id));
            strlcpy(g->name, g->id, sizeof(g->name));
            name_own = phrases_own = false;
            continue;
        }
        char *eq = strchr(line, '=');
        if (!g || !eq) {
            continue;
        }
        *eq = '\0';
        char *key = line, *value = eq + 1;
        trim(key);
        trim(value);
        if (strcmp(key, key_name) == 0 || (strcmp(key, "name_en") == 0 && !name_own)) {
            strlcpy(g->name, value, sizeof(g->name));
            name_own |= strcmp(key, key_name) == 0;
        } else if (strcmp(key, key_phrases) == 0 || (strcmp(key, "phrases_en") == 0 && !phrases_own)) {
            strlcpy(g->phrases, value, sizeof(g->phrases));
            phrases_own |= strcmp(key, key_phrases) == 0;
        } else if (strcmp(key, "first") == 0) {
            free(g->first);
            g->first = strdup(value);
        } else if (strcmp(key, "rules") == 0) {
            free(g->rules);
            g->rules = strdup(value);
        }
    }
    free(line);
    fclose(f);
    ESP_LOGI(TAG, "%d game(s) in %s", s_game_count, GAMES_FILE);
}

static int game_find(const char *id)
{
    for (int i = 0; i < s_game_count; i++) {
        if (strcmp(s_games[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

/* A phrase to compare: lower case, without accents (UTF-8 Latin letters) nor punctuation, single spaces */
static void fold(const char *in, char *out, size_t size)
{
    static const char from[] = "àáâãäåçèéêëìíîïñòóôõöùúûüýÿ";
    static const char to[] = "aaaaaaceeeeiiiinooooouuuuyy";
    size_t n = 0;
    bool space = false;
    for (const unsigned char *p = (const unsigned char *)in; *p && n + 2 < size; p++) {
        char c = 0;
        if (p[0] == 0xC3 && p[1]) {
            unsigned char second = p[1] | 0x20;
            for (size_t i = 0; i + 1 < sizeof(from) - 1; i += 2) {
                if ((unsigned char)from[i + 1] == second) {
                    c = to[i / 2];
                    break;
                }
            }
            p++;
            if (!c) {
                continue;
            }
        } else if (isalnum(*p)) {
            c = (char)tolower(*p);
        } else if (*p >= 0x80) {
            continue;
        }
        if (!c) {
            space = n > 0;      // punctuation, a dash, a blank: one space
            continue;
        }
        if (space) {
            out[n++] = ' ';
            space = false;
        }
        out[n++] = c;
    }
    out[n] = '\0';
}

/* The game whose phrase begins what was said, -1 if none */
static int game_of(const char *text)
{
    char said[200];
    fold(text, said, sizeof(said));
    for (int i = 0; i < s_game_count; i++) {
        char phrases[400];
        strlcpy(phrases, s_games[i].phrases, sizeof(phrases));
        for (char *p = strtok(phrases, "|"); p; p = strtok(NULL, "|")) {
            char phrase[100];
            fold(p, phrase, sizeof(phrase));
            size_t len = strlen(phrase);
            if (len && strncmp(said, phrase, len) == 0 && (said[len] == '\0' || said[len] == ' ')) {
                return i;
            }
        }
    }
    return -1;
}

/* ---------- network ---------- */

static void send_to(uint32_t ip, cJSON *msg)
{
    char *text = cJSON_PrintUnformatted(msg);
    if (!text || s_sock < 0) {
        free(text);
        return;
    }
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(GAME_PORT),
        .sin_addr.s_addr = ip,
    };
    for (int i = 0; i < GAME_SEND_TIMES; i++) {
        sendto(s_sock, text, strlen(text), 0, (struct sockaddr *)&to, sizeof(to));
    }
    free(text);
}

static cJSON *msg_new(const char *type)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "t", type);
    char id[20];
    snprintf(id, sizeof(id), "%llx", (unsigned long long)s_my_id);
    cJSON_AddStringToObject(m, "id", id);
    cJSON_AddNumberToObject(m, "n", s_nonce);
    return m;
}

/* A message to the other BOX: this type, a turn, a text (or none) */
static void send_peer(const char *type, int turn, const char *text)
{
    cJSON *m = msg_new(type);
    cJSON_AddNumberToObject(m, "turn", turn);
    if (text) {
        cJSON_AddStringToObject(m, "text", text);
    }
    send_to(s_peer_ip, m);
    cJSON_Delete(m);
}

static void net_task(void *arg)
{
    char *buf = heap_caps_malloc(GAME_TEXT_MAX + 512, MALLOC_CAP_SPIRAM);
    for (;;) {
        struct sockaddr_in from;
        socklen_t len = sizeof(from);
        int n = buf ? recvfrom(s_sock, buf, GAME_TEXT_MAX + 511, 0, (struct sockaddr *)&from, &len) : -1;
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        buf[n] = '\0';
        cJSON *json = cJSON_Parse(buf);
        const cJSON *type = cJSON_GetObjectItem(json, "t");
        const cJSON *id = cJSON_GetObjectItem(json, "id");
        uint64_t sender = cJSON_IsString(id) ? strtoull(id->valuestring, NULL, 16) : 0;
        if (cJSON_IsString(type) && sender && sender != s_my_id) {
            game_msg_t msg = { 0 };
            strlcpy(msg.type, type->valuestring, sizeof(msg.type));
            msg.from = sender;
            msg.ip = from.sin_addr.s_addr;
            const cJSON *v = cJSON_GetObjectItem(json, "n");
            msg.nonce = cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 0;
            v = cJSON_GetObjectItem(json, "turn");
            msg.turn = cJSON_IsNumber(v) ? v->valueint : 0;
            v = cJSON_GetObjectItem(json, "tone");
            msg.tone = cJSON_IsNumber(v) ? v->valueint : -1;
            v = cJSON_GetObjectItem(json, "game");
            strlcpy(msg.game, cJSON_IsString(v) ? v->valuestring : "", sizeof(msg.game));
            v = cJSON_GetObjectItem(json, "text");
            msg.text = cJSON_IsString(v) ? strdup(v->valuestring) : NULL;
            if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
                free(msg.text);
            } else {
                assistant_save_soon();      // the main task, at once
            }
        }
        cJSON_Delete(json);
    }
}

void game_init(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    for (int i = 0; i < 6; i++) {
        s_my_id = (s_my_id << 8) | mac[i];
    }
    games_install();
    s_queue = xQueueCreate(16, sizeof(game_msg_t));
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int yes = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(GAME_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (s_sock < 0 || bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "UDP port %d not opened", GAME_PORT);
        return;
    }
    // Small: it only reads the messages and gives them to the main task
    if (xTaskCreatePinnedToCore(net_task, "game_net", 3072, NULL, 4, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
    }
}

/* ---------- the game ---------- */

bool game_active(void)
{
    return s_state != G_IDLE;
}

/* My personality: PERSO_<XX>.TXT (Game_tone = persona), else one of the list, not the other's */
static void tone_choose(int other)
{
    char *setting = settings_read_value("Game_tone");
    bool persona = setting && strcasecmp(setting, "persona") == 0;
    free(setting);
    free(s_tone_text);
    s_tone_text = NULL;
    s_my_tone = -1;
    if (persona) {
        asprintf(&s_tone_text, "%s %s", persona_get(PERSONA_IDENTITY), persona_get(PERSONA_PERSONALITY));
        return;
    }
    do {
        s_my_tone = (int)(esp_random() % TONES_COUNT);
    } while (s_my_tone == other && TONES_COUNT > 1);
    s_tone_text = strdup(s_tones[s_my_tone]);
}

static void game_reset(void)
{
    s_state = G_IDLE;
    s_game = -1;
    s_peer_id = 0;
    s_peer_ip = 0;
    s_turns = 0;
    s_peer_done = 0;
    cJSON_Delete(s_history);
    s_history = NULL;
    free(s_ready_line);
    s_ready_line = NULL;
}

/* The end, said on screen (and spoken), back to sleep */
static void game_end(const char *why)
{
    ESP_LOGI(TAG, "Game over: %s", why);
    game_reset();
    if (why && why[0]) {
        assistant_say(why);
    } else {
        ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, 0);
    }
}

static void playing_start(int game, uint32_t nonce, bool starter, uint64_t peer, uint32_t ip)
{
    s_game = game;
    s_nonce = nonce;
    s_starter = starter;
    s_peer_id = peer;
    s_peer_ip = ip;
    s_turns = 0;
    s_peer_done = 0;
    cJSON_Delete(s_history);
    s_history = cJSON_CreateArray();
    free(s_ready_line);
    s_ready_line = NULL;
    char *max = settings_read_value("Game_turns_max");
    s_turns_max = max ? atoi(max) : GAME_TURNS_DEFAULT;
    s_turns_max = s_turns_max < 2 ? 2 : s_turns_max > 100 ? 100 : s_turns_max;
    free(max);
    s_state = G_PLAYING;
    s_since = esp_timer_get_time();
    radio_suspend();
    char title[96];
    snprintf(title, sizeof(title), app_lang_get()->game_title, s_games[game].name);
    assistant_show(title);
    ESP_LOGI(TAG, "%s: playing %s, %s, %d lines at most", s_games[game].id, starter ? "starting" : "answering",
             s_tone_text ? s_tone_text : "-", s_turns_max);
}

bool game_try_start(const char *text)
{
    if (!s_queue || s_sock < 0) {
        return false;
    }
    games_load();
    int game = game_of(text);
    if (game < 0) {
        return false;
    }
    if (wifi_connected_already() != WIFI_STATUS_CONNECTED_OK) {
        return false;
    }
    game_reset();
    s_game = game;
    s_nonce = esp_random();
    tone_choose(-1);
    s_state = G_INVITING;
    s_since = esp_timer_get_time();
    // To every BOX of the network
    cJSON *m = msg_new("invite");
    cJSON_AddStringToObject(m, "game", s_games[game].id);
    cJSON_AddNumberToObject(m, "tone", s_my_tone);
    send_to(htonl(INADDR_BROADCAST), m);
    cJSON_Delete(m);
    assistant_show(app_lang_get()->game_searching);
    ESP_LOGI(TAG, "%s: looking for the other BOX", s_games[game].id);
    return true;
}

bool game_stop(void)
{
    if (s_state == G_IDLE) {
        return false;
    }
    s_stop_requested = true;
    tts_stream_cancel();
    return true;
}

/* The question to the model: the rules, then the lines so far */
static char *line_make(void)
{
    const game_def_t *g = &s_games[s_game];
    const app_lang_t *lang = app_lang_get();
    char *system = NULL;
    asprintf(&system, "You are a voice assistant playing a game out loud with another voice assistant (another "
             "device), in front of the user. Speak only the language whose code is '%s'. Your character: %s; stay in "
             "character. Game: %s. Rules: %s Each of your lines is spoken aloud: one to three short sentences, no "
             "lists, no emojis, no markdown, no stage directions, never speak for the other player.",
             lang->code, s_tone_text ? s_tone_text : "yourself", g->name, g->rules ? g->rules : "");
    char *question = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&question, &size);
    if (out) {
        if (cJSON_GetArraySize(s_history) > 0) {
            fputs("The game so far, oldest first:\n", out);
            const cJSON *item;
            cJSON_ArrayForEach(item, s_history) {
                fprintf(out, "%s: %s\n", cJSON_IsTrue(cJSON_GetObjectItem(item, "me")) ? "You" : "The other",
                        cJSON_GetObjectItem(item, "text")->valuestring);
            }
            fputs("\nSay your next line.", out);
        } else {
            fprintf(out, "%s In this first line, also say in a few words who you are (your character).",
                    g->first ? g->first : "Start the game.");
        }
        if (s_turns + 1 >= s_turns_max) {
            fputs(" This is the last line of the game: end it in one or two sentences, in character.", out);
        }
        fclose(out);
    }
    char *answer = NULL;
    llm_error_t err = system && question ? llm_ask_apart(system, question, &answer) : LLM_ERR_OTHER;
    free(system);
    free(question);
    if (err != LLM_OK) {
        free(answer);
        return NULL;
    }
    return answer;
}

static volatile bool s_spoken = false;

static void on_speech(tts_event_t event)
{
    if (event == TTS_EVENT_DONE) {
        s_spoken = true;
    }
}

/* My line: sent first (the other prepares its answer meanwhile), shown, spoken, then "done" */
static void line_say(char *text)
{
    int turn = s_turns + 1;
    send_peer("line", turn, text);
    assistant_show(text);
    s_spoken = false;
    tts_stream_begin(on_speech);
    tts_stream_say(text);
    tts_stream_end();
    int64_t t0 = esp_timer_get_time();
    while (!s_spoken && !s_stop_requested && esp_timer_get_time() - t0 < 60 * 1000000LL) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    cJSON *item = cJSON_CreateObject();
    cJSON_AddBoolToObject(item, "me", true);
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(s_history, item);
    s_turns = turn;
    s_since = esp_timer_get_time();
    if (!s_stop_requested) {
        send_peer("done", turn, NULL);
    }
}

/* A message of the network task */
static void message(game_msg_t *m)
{
    if (strcmp(m->type, "invite") == 0) {
        games_load();
        int game = game_find(m->game);
        bool join = false;
        if (game >= 0 && s_state == G_IDLE && ui_ctrl_is_asleep()) {
            join = true;        // asleep: it takes part
        } else if (game >= 0 && s_state == G_INVITING && m->from < s_my_id) {
            join = true;        // both heard it: the lower id starts, the other answers
        }
        if (join) {
            tone_choose(m->tone);
            playing_start(game, m->nonce, false, m->from, m->ip);
            cJSON *j = msg_new("join");
            cJSON_AddNumberToObject(j, "tone", s_my_tone);
            send_to(s_peer_ip, j);
            cJSON_Delete(j);
        }
    } else if (strcmp(m->type, "join") == 0) {
        if (s_state == G_INVITING && m->nonce == s_nonce) {
            if (s_my_tone >= 0 && m->tone == s_my_tone) {
                tone_choose(m->tone);       // the same drawn by both: another one
            }
            playing_start(s_game, s_nonce, true, m->from, m->ip);
        }
    } else if (s_state == G_PLAYING && m->nonce == s_nonce && m->from == s_peer_id) {
        s_since = esp_timer_get_time();
        if (strcmp(m->type, "line") == 0 && m->turn == s_turns + 1 && m->text) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddBoolToObject(item, "me", false);
            cJSON_AddStringToObject(item, "text", m->text);
            cJSON_AddItemToArray(s_history, item);
            s_turns = m->turn;
            assistant_show(m->text);
        } else if (strcmp(m->type, "done") == 0) {
            s_peer_done = m->turn > s_peer_done ? m->turn : s_peer_done;
        } else if (strcmp(m->type, "stop") == 0) {
            tts_stream_cancel();
            game_end(app_lang_get()->game_over);
        }
    }
    free(m->text);
}

void game_poll(void)
{
    game_msg_t m;
    while (s_queue && xQueueReceive(s_queue, &m, 0) == pdTRUE) {
        message(&m);
    }
    if (s_stop_requested) {
        s_stop_requested = false;
        if (s_state == G_PLAYING) {
            send_peer("stop", s_turns, NULL);
        }
        if (s_state != G_IDLE) {
            ESP_LOGI(TAG, "Stopped by the wake word or a touch");
            game_reset();
        }
        return;
    }
    if (s_state == G_INVITING && esp_timer_get_time() - s_since > GAME_INVITE_MS * 1000LL) {
        game_end(app_lang_get()->game_no_peer);
        return;
    }
    if (s_state != G_PLAYING) {
        return;
    }
    // Whose line is next: the one starting says the odd ones
    bool mine = ((s_turns + 1) % 2 == 1) == s_starter;
    if (s_turns >= s_turns_max) {
        game_end(app_lang_get()->game_over);
        return;
    }
    if (!mine) {
        if (esp_timer_get_time() - s_since > GAME_SILENCE_MS * 1000LL) {
            game_end(app_lang_get()->game_peer_left);
        }
        return;
    }
    // My line: made at once (while the other may still be speaking its own), said once the other is done
    if (!s_ready_line) {
        s_ready_line = line_make();
        if (!s_ready_line) {
            game_end(app_lang_get()->game_peer_left);
            return;
        }
    }
    if (s_turns > 0 && s_peer_done < s_turns) {
        if (esp_timer_get_time() - s_since > GAME_SILENCE_MS * 1000LL) {
            game_end(app_lang_get()->game_peer_left);
        }
        return;     // the other is still speaking
    }
    char *text = s_ready_line;
    s_ready_line = NULL;
    line_say(text);
    free(text);
    if (s_turns >= s_turns_max && !s_stop_requested) {
        send_peer("stop", s_turns, NULL);
        game_end(app_lang_get()->game_over);
    }
}
