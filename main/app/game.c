/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Games between BOX: see game.h.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "settings.h"
#include "file_lang.h"
#include "file_persona.h"
#include "llm.h"
#include "tts_stream.h"
#include "radio.h"
#include "app_wifi.h"
#include "app_ui_ctrl.h"
#include "app_sr.h"
#include "file_state.h"
#include "game.h"
#include "game_internal.h"

static const char *TAG = "game";

/*
 * Looking for the other BOX: the claims of the first line collected this long; alone, this one waits for the others
 * up to GAME_INVITE_MS (a BOX that heard the phrase too but transcribes it later), else "no other BOX"
 */
#define GAME_START_WAIT_MS  2500
#define GAME_INVITE_MS      8000
/* A line heard: the other BOX' claims of the next one awaited this long after the first one */
#define GAME_HEARD_WAIT_MS  3000
/* Nobody claimed the next line this long after the end of a line: the lowest id goes on */
#define GAME_DONE_WAIT_MS   12000
/* The BOX elected silent this long: the game ends (a line in the reasoning mode may take up to 2 minutes) */
#define GAME_SPEAK_WAIT_MS  150000
/* A listening waits this long for the speech of the line (the speaker's voice comes a second after its "speak") */
#define GAME_LISTEN_WAIT_MS 15000
/* The speaker done: a listening started again after its pause stops after this; the first one after this */
#define GAME_MORE_WAIT_MS   2500
#define GAME_TAIL_WAIT_MS   8000
/* A line not ended this long after it started: the game ends */
#define GAME_SILENCE_MS     90000
#define GAME_PLAYERS_MAX    8
#define GAMES_USED_SHOWN    40
#define GAME_TURNS_DEFAULT  20

/* main.c */
extern void assistant_show(const char *text);
/* main.c: said, then asleep (no listening after it) */
extern void assistant_say_final(const char *text);
extern void assistant_say(const char *text);
extern void assistant_save_soon(void);
extern void assistant_reply_stop(void);

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

typedef enum {
    G_IDLE,
    G_STARTING,             /* the BOX that heard the phrase claim the first line, the others join */
    G_SPEAKING,             /* this BOX makes and says its line */
    G_LISTENING,            /* another one speaks: listened to with the microphone */
    G_ELECTING,             /* a line said: who says the next one (the first to have heard it) */
} game_state_t;

/* A BOX of the game, and its claim of the next line */
typedef struct {
    uint64_t id;
    int claim_turn;         /* its claim: of the line after this many lines (-1: none) */
    int64_t ts;             /* the claim's time (ms since 1970) */
    int64_t seen_us;        /* when this BOX got it (esp_timer) */
    int round;
    uint32_t nonce;         /* the start claims: the game's id and lines if it is elected */
    int turns_max;
} player_t;

static uint64_t s_my_id = 0;
static bool s_net_ready = false;

static volatile game_state_t s_state = G_IDLE;
static volatile bool s_stop_requested = false;
static int s_game = -1;
static uint32_t s_nonce = 0;
static player_t s_players[GAME_PLAYERS_MAX];
static int s_player_count = 0;
static int s_turns = 0;             /* lines said so far (all BOX) */
static int s_turns_max = GAME_TURNS_DEFAULT;
static int s_my_tone = -1;
static uint32_t s_tones_seen = 0;   /* the characters drawn by the others (bits) */
static char *s_tone_text = NULL;    /* the personality given to the model */
static cJSON *s_history = NULL;     /* [{"id":"..","me":true,"tag":"GUESS","text":"..."}] */
static int64_t s_since = 0;         /* the start of the current wait (esp_timer, us) */

static int64_t s_reclaim_us = 0;    /* a tie: this BOX claims again at this time (0: no) */
static bool s_refusal_said = false; /* the moderation of the voice explained aloud (once a game) */

/* The line being listened to */
static uint64_t s_speaker = 0;     /* the BOX speaking the line listened to */
static uint64_t s_last_speaker = 0; /* the BOX of the last line: it does not claim the next one */
static char s_line_tag[16] = "";
static char *s_heard = NULL;        /* what was transcribed (several listenings if the speaker paused) */
static bool s_done_seen = false;    /* the speaker finished */
static char *s_done_text = NULL;    /* the line as the speaker sent it: used if nothing was heard */
static volatile bool s_listen_on = false;   /* a listening of the game is asked or under way */
static bool s_listen_more = false;  /* a listening started again after a pause of the speaker */
static int64_t s_done_us = 0;

/*
 * A claim received while busy (the phrase heard here too, still transcribed): joined when the phrase is transcribed,
 * within GAME_INVITE_MS
 */
static game_msg_t s_claim_seen;
static int64_t s_claim_seen_us = 0;

/* ---------- network ---------- */

static cJSON *msg_new(const char *type)
{
    return game_net_msg(type, s_nonce);
}

static void send_all(cJSON *m)
{
    game_net_send(m);
}

static void send_simple(const char *type, int turn, const char *text)
{
    cJSON *m = msg_new(type);
    cJSON_AddNumberToObject(m, "turn", turn);
    if (text) {
        cJSON_AddStringToObject(m, "text", text);
    }
    send_all(m);
}

void game_init(void)
{
    games_init();
    if (game_net_init()) {
        s_net_ready = true;
    }
    s_my_id = game_net_id();
}

/* ---------- the game ---------- */

bool game_active(void)
{
    return s_state != G_IDLE;
}

bool game_listening(void)
{
    return s_listen_on;
}

void game_heard(char *text)
{
    game_msg_t msg = { 0 };
    strlcpy(msg.type, "local", sizeof(msg.type));
    msg.from = s_my_id;
    msg.text = text;
    if (!game_net_post(&msg)) {
        free(text);
        s_listen_on = false;
    }
}

/* My personality: PERSO_<XX>.TXT (Game_tone = persona), else one of the list, not one drawn by another BOX */
static void tone_choose(void)
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
    // This game's characters (the creation: experts), else the firmware's list
    const char *list = s_game >= 0 ? game_def(s_game)->characters : NULL;
    int count = game_list_count(list);
    int tries = 0;
    do {
        s_my_tone = (int)(esp_random() % (count ? count : TONES_COUNT));
    } while ((s_tones_seen & (1u << (s_my_tone % 32))) && ++tries < 50);
    if (count) {
        char character[120];
        game_list_item(list, s_my_tone, character, sizeof(character));
        s_tone_text = strdup(character);
    } else {
        s_tone_text = strdup(s_tones[s_my_tone]);
    }
}

/* ---------- the BOX of the game and their claims ---------- */

static player_t *player_find(uint64_t id)
{
    for (int i = 0; i < s_player_count; i++) {
        if (s_players[i].id == id) {
            return &s_players[i];
        }
    }
    return NULL;
}

static player_t *player_add(uint64_t id)
{
    player_t *p = player_find(id);
    if (!p && s_player_count < GAME_PLAYERS_MAX) {
        p = &s_players[s_player_count++];
        memset(p, 0, sizeof(*p));
        p->id = id;
        p->claim_turn = -1;
        ESP_LOGI(TAG, "player %d: %llx%s", s_player_count, (unsigned long long)id, id == s_my_id ? " (this BOX)" : "");
    }
    return p;
}

/* Its number, as the other lines are named for the model ("Player 2") */
static int player_number(uint64_t id)
{
    for (int i = 0; i < s_player_count; i++) {
        if (s_players[i].id == id) {
            return i + 1;
        }
    }
    return 0;
}

/* A claim of the line after s_turns (turn 0: the first one), its time kept: the earliest speaks */
static void claim_take(const game_msg_t *m)
{
    player_t *p = player_add(m->from);
    if (!p) {
        return;
    }
    if (p->claim_turn != m->turn || m->round >= p->round) {
        if (p->claim_turn != m->turn) {
            p->seen_us = esp_timer_get_time();
        }
        p->claim_turn = m->turn;
        p->ts = m->ts;
        p->round = m->round;
        p->nonce = m->nonce;
        p->turns_max = m->turns_max;
    }
}

/* This BOX claims the next line: when it heard the phrase (start) or the line said (heard) */
static void claim_send(int round)
{
    game_msg_t mine = {
        .from = s_my_id,
        .nonce = s_nonce,
        .turn = s_turns,
        .round = round,
        .ts = game_now_ms(),
        .turns_max = s_turns_max,
    };
    claim_take(&mine);
    cJSON *m = msg_new(s_state == G_STARTING ? "claim" : "heard");
    cJSON_AddNumberToObject(m, "turn", s_turns);
    cJSON_AddNumberToObject(m, "round", round);
    cJSON_AddNumberToObject(m, "ts", (double)mine.ts);
    if (s_state == G_STARTING) {
        cJSON_AddStringToObject(m, "game", game_def(s_game)->id);
        cJSON_AddNumberToObject(m, "max", s_turns_max);
        cJSON_AddNumberToObject(m, "tone", s_my_tone);
    }
    send_all(m);
    ESP_LOGI(TAG, "claim of line %d (round %d)", s_turns + 1, round);
}

/* ---------- the lines ---------- */

/* The tags this line may have (next: after the last line's tag), "" if free */
static void next_tags(char *out, size_t size)
{
    out[0] = '\0';
    const cJSON *last = cJSON_GetArrayItem(s_history, cJSON_GetArraySize(s_history) - 1);
    const cJSON *tag = last ? cJSON_GetObjectItem(last, "tag") : NULL;
    if (cJSON_IsString(tag)) {
        game_rule_of(game_def(s_game)->next, tag->valuestring, out, size);
    }
}

/*
 * turns: "GUESS>RIDDLE WRONG; RIGHT>GUESS": after a line tagged GUESS, the BOX of the last line tagged RIDDLE or
 * WRONG says the next one (no claim). 0 when the line's tag has no such rule (the earliest claim speaks).
 */
static uint64_t owner_after(const char *tag)
{
    char right[64];
    if (!tag[0] || !game_rule_of(game_def(s_game)->turns, tag, right, sizeof(right))) {
        return 0;
    }
    // The last line (before this one) whose tag is on the right
    for (int i = cJSON_GetArraySize(s_history) - 2; i >= 0; i--) {
        const cJSON *item = cJSON_GetArrayItem(s_history, i);
        const cJSON *t = cJSON_GetObjectItem(item, "tag");
        if (cJSON_IsString(t) && game_tag_in(right, strlen(right), t->valuestring)) {
            const cJSON *id = cJSON_GetObjectItem(item, "id");
            return cJSON_IsString(id) ? strtoull(id->valuestring, NULL, 16) : 0;
        }
    }
    return 0;
}

static void history_add(uint64_t id, const char *tag, const char *text)
{
    cJSON *item = cJSON_CreateObject();
    char hex[20];
    snprintf(hex, sizeof(hex), "%llx", (unsigned long long)id);
    cJSON_AddStringToObject(item, "id", hex);
    cJSON_AddBoolToObject(item, "me", id == s_my_id);
    cJSON_AddStringToObject(item, "tag", tag);
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(s_history, item);
}

static void listen_stop(void)
{
    if (s_listen_on) {
        s_listen_on = false;
        app_sr_cancel();
    }
}

/*
 * During a game the Wi-Fi does not save power: asleep between beacons, the radio missed broadcasts (a "speak" lost,
 * the game stuck)
 */
static void wifi_awake(bool awake)
{
    static bool s_awake = false;
    if (awake != s_awake) {
        s_awake = awake;
        esp_wifi_set_ps(awake ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
        ESP_LOGI(TAG, "Wi-Fi power save %s", awake ? "off" : "on");
    }
}

static void game_reset(void)
{
    listen_stop();
    wifi_awake(false);
    s_state = G_IDLE;
    s_game = -1;
    s_player_count = 0;
    s_turns = 0;
    s_speaker = 0;
    s_last_speaker = 0;
    s_tones_seen = 0;
    s_reclaim_us = 0;
    s_refusal_said = false;
    s_done_seen = false;
    free(s_heard);
    s_heard = NULL;
    free(s_done_text);
    s_done_text = NULL;
    cJSON_Delete(s_history);
    s_history = NULL;
}

/* The end, said on screen (and spoken), back to sleep without listening; NULL: quietly */
static void game_end(const char *why)
{
    ESP_LOGI(TAG, "Game over: %s", why ? why : "(quietly)");     // NULL in a printf: a crash
    game_reset();
    if (why && why[0]) {
        assistant_say_final(why);
    } else {
        ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, 0);
    }
}

/* Ready to play this game: the claims of its first line to come */
static void start_setup(int game, uint32_t nonce)
{
    game_reset();
    s_game = game;
    s_nonce = nonce;
    s_history = cJSON_CreateArray();
    char *max = settings_read_value("Game_turns_max");
    s_turns_max = max ? atoi(max) : GAME_TURNS_DEFAULT;
    if (game_def(game)->lines > 0) {
        s_turns_max = game_def(game)->lines;    // a guessing game: its own length
    }
    s_turns_max = s_turns_max < 2 ? 2 : s_turns_max > 100 ? 100 : s_turns_max;
    free(max);
    tone_choose();
    player_add(s_my_id);
    wifi_awake(true);
    s_state = G_STARTING;
    s_since = esp_timer_get_time();
    radio_suspend();
}

bool game_try_start(const char *text)
{
    if (!s_net_ready) {
        return false;
    }
    int game = game_phrase(text);
    if (game < 0) {
        return false;
    }
    if (wifi_connected_already() != WIFI_STATUS_CONNECTED_OK) {
        return false;
    }
    // Started by the main task, which plays the game (this is the SR handler task)
    game_msg_t msg = { .from = s_my_id, .turn = game };
    strlcpy(msg.type, "start", sizeof(msg.type));
    return game_net_post(&msg);
}

/* The phrase of a game heard here: this BOX claims its first line */
static void game_start(int game)
{
    if (s_state == G_STARTING && game == s_game) {
        claim_send(0);          // joined as asleep, then heard the phrase too
        return;
    }
    if (s_state != G_IDLE && s_state != G_STARTING) {
        return;                 // a game going on
    }
    bool seen = s_claim_seen_us && esp_timer_get_time() - s_claim_seen_us < GAME_INVITE_MS * 1000LL &&
                game_find(s_claim_seen.game) == game;
    start_setup(game, seen ? s_claim_seen.nonce : esp_random());
    if (seen) {
        // Another BOX heard it too and claims already: this one claims in the same election
        ESP_LOGI(TAG, "%s: the claim of another BOX already received", game_def(game)->id);
        claim_take(&s_claim_seen);
    }
    s_claim_seen_us = 0;
    claim_send(0);
    assistant_show(app_lang_get()->game_organizing);
    ESP_LOGI(TAG, "%s: looking for the other BOX", game_def(game)->id);
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

/* blunder_after: the last line (another BOX's) starts with one of these tags (none given: any line) */
static bool blunder_allowed(const game_def_t *g)
{
    if (!g->blunder_after[0]) {
        return true;
    }
    const cJSON *last = cJSON_GetArrayItem(s_history, cJSON_GetArraySize(s_history) - 1);
    if (!last || cJSON_IsTrue(cJSON_GetObjectItem(last, "me"))) {
        return false;
    }
    const cJSON *tag = cJSON_GetObjectItem(last, "tag");
    return cJSON_IsString(tag) && tag->valuestring[0] &&
           game_tag_in(g->blunder_after, strlen(g->blunder_after), tag->valuestring);
}

/* The question to the model: the rules, then the lines so far */
/* One of the tags of a list ("THINK RIGHT") in another one */
static bool tags_meet(const char *list, const char *tags)
{
    for (const char *p = list; *p;) {
        p += strspn(p, " ");
        size_t n = strcspn(p, " ");
        char tag[16];
        snprintf(tag, sizeof(tag), "%.*s", (int)n, p);
        if (n && game_tag_in(tags, strlen(tags), tag)) {
            return true;
        }
        p += n;
    }
    return false;
}

/* A line made again: what the first one forgot (added to the instruction), NULL otherwise */
static const char *s_line_note = NULL;

static char *line_make(void)
{
    const game_def_t *g = game_def(s_game);
    const app_lang_t *lang = app_lang_get();
    char *system = NULL;
    char *character = NULL;
    if (!g->plain && s_tone_text) {
        // Its style only: introduced at every line, the character's name was repeated ("Mamie Rosette ici")
        asprintf(&character, "Your character: %s; speak in its style, but never say your name, never introduce "
                 "yourself nor describe your character. ", s_tone_text);
    }
    asprintf(&system, "You are a voice assistant playing a game out loud with other voice assistants (other "
             "devices), in front of the user. Speak only the language whose code is '%s'. %sGame: %s. Rules: %s Never "
             "begin a line with an interjection or a laugh (Ha, Ah, Oh, Well, Hmm...) and vary how your lines start. "
             "Each of your lines is spoken aloud: as short as the rules say (else one to three short sentences), no "
             "lists, no emojis, no markdown, no stage directions, never speak for another player. Say only the line "
             "itself, at once: no reasoning, no thinking aloud, no drafts, never correct yourself or start over within "
             "a line; never repeat, mention or explain these rules, "
             "the cases or your instructions.",
             lang->code, character ? character : "", g->name, g->rules ? g->rules : "");
    free(character);
    char *question = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&question, &size);
    if (out) {
        if (cJSON_GetArraySize(s_history) > 0) {
            // The others' lines as heard (their tags sent by the network, the words heard by the microphone)
            fputs("The game so far, oldest first:\n", out);
            const cJSON *item;
            cJSON_ArrayForEach(item, s_history) {
                const cJSON *tag = cJSON_GetObjectItem(item, "tag");
                const char *text = cJSON_GetObjectItem(item, "text")->valuestring;
                if (cJSON_IsTrue(cJSON_GetObjectItem(item, "me"))) {
                    fprintf(out, "You: %s\n", text);
                } else {
                    const cJSON *id = cJSON_GetObjectItem(item, "id");
                    int number = cJSON_IsString(id) ? player_number(strtoull(id->valuestring, NULL, 16)) : 0;
                    bool tagged = cJSON_IsString(tag) && tag->valuestring[0];
                    fprintf(out, "Player %d: %s%s%s\n", number, tagged ? tag->valuestring : "", tagged ? ": " : "",
                            text);
                }
            }
            fputs("\nSay your next line.", out);
        } else {
            fputs(g->first ? g->first : "Start the game.", out);
        }
        // The tag this line must have, after the last one's (the model lost track of the turns)
        char tags[32];
        next_tags(tags, sizeof(tags));
        if (tags[0]) {
            if (strchr(tags, ' ')) {
                fprintf(out, " This line must start with one of these tags: %s (choose the one that applies).", tags);
            } else {
                fprintf(out, " This line must start with the tag %s:", tags);
            }
        }
        // A theme drawn (each line: the model uses it when it asks or opens something new), and what was used
        char *used = game_used_read(g->id);
        bool opening = cJSON_GetArraySize(s_history) == 0;
        bool themed = !g->theme_for[0] || opening ||
                      (strcmp(g->theme_for, "first") != 0 && tags[0] && tags_meet(g->theme_for, tags));
        if (g->themes && g->themes[0] && themed) {
            char theme[64];
            game_theme_draw(g, used, theme, sizeof(theme));
            const char *rule = g->theme_rule && strstr(g->theme_rule, "%s") ? g->theme_rule
                               : "Theme of anything new in this line: %s.";
            const char *at = strstr(rule, "%s");
            fprintf(out, " %.*s%s%s", (int)(at - rule), rule, theme, at + 2);
            ESP_LOGI(TAG, "theme: %s", theme);
            if (!g->secret[0] && cJSON_GetArraySize(s_history) == 0) {
                game_used_add(g->id, theme);     // a debate's topic: not drawn again soon
            }
        }
        if (used && g->secret[0]) {
            // The last answers, one line each: never used again
            fputs(" Never use one of these answers, already used in earlier games:", out);
            int lines = 0;
            for (const char *p = used; *p; p++) {
                lines += *p == '\n';
            }
            const char *p = used;
            for (; lines > GAMES_USED_SHOWN; lines--) {
                p = strchr(p, '\n') + 1;
            }
            for (bool first = true; *p; first = false) {
                const char *nl = strchr(p, '\n');
                size_t n = nl ? (size_t)(nl - p) : strlen(p);
                fprintf(out, "%s %.*s", first ? "" : ",", (int)n, p);
                p += n + (nl != NULL);
            }
            fputs(".", out);
        }
        free(used);
        if (g->blunder_rule && g->blunder > 0 && blunder_allowed(g) && (int)(esp_random() % 100) < g->blunder) {
            ESP_LOGI(TAG, "this line: %s", "blunder_rule");
            fprintf(out, " %s", g->blunder_rule);
        }
        if (s_line_note) {
            fprintf(out, " %s", s_line_note);
        }
        if (s_turns + 1 >= s_turns_max) {
            fprintf(out, " %s", g->last && g->last[0] ? g->last
                    : "This is the last line of the game: end it in one or two sentences, in character.");
        }
        fclose(out);
    }
    char *answer = NULL;
    llm_error_t err = system && question ? llm_ask_apart(system, question, &answer, g->reasoning) : LLM_ERR_OTHER;
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
    if (event == TTS_EVENT_FIRST_AUDIO) {
        ui_ctrl_reply_set_audio_start_flag(true);   // the line scrolls with its speech
    } else if (event == TTS_EVENT_DONE) {
        s_spoken = true;
    }
}

/* short_after: a line starting with this tag keeps only its first sentence (a riddle guessed: "Correct !" and
   nothing more, the one who found it asks the next one) */
static void line_shorten(char *text, const char *short_after)
{
    size_t tag_len = 0;
    char *body = (char *)game_tag_skip(text, &tag_len);
    const char *tag = text + strspn(text, " \n");
    if (!short_after[0] || tag_len != strlen(short_after) || strncmp(tag, short_after, tag_len) != 0) {
        return;
    }
    for (char *p = body; *p; p++) {
        if (*p == '.' || *p == '!' || *p == '?') {
            if (p[1] && p[1] != '.' && p[1] != '!' && p[1] != '?') {
                ESP_LOGI(TAG, "%s: only its first sentence kept", short_after);
                p[1] = '\0';
            }
            break;
        }
    }
}

/*
 * The model sometimes thinks aloud before its line ("The last line was a RIGHT line, so I must...", several drafts):
 * with tags, only the last paragraph starting with one is kept
 */
static void line_clean(char *text)
{
    const game_def_t *g = game_def(s_game);
    if (!g->short_after[0] && !g->turns[0]) {
        // A game without tags: its reasoning comes in paragraphs before the line ("Ce n'est pas le début du
        // débat, donc je réponds...", a blank line, then the line): only the last paragraph is kept
        size_t len = strlen(text);
        while (len && (text[len - 1] == '\n' || text[len - 1] == ' ')) {
            text[--len] = '\0';
        }
        char *last = NULL;
        for (char *p = strstr(text, "\n\n"); p; p = strstr(p + 1, "\n\n")) {
            last = p;
        }
        if (last) {
            last += strspn(last, "\n ");
            ESP_LOGW(TAG, "the model thought aloud: only its last paragraph kept");
            memmove(text, last, strlen(last) + 1);
        }
        return;
    }
    char *keep = NULL;
    for (char *p = text; *p; p++) {
        if (p == text || p[-1] == '\n') {
            size_t tag_len = 0;
            game_tag_skip(p, &tag_len);
            // The secret ("ANSWER:" on a line of its own) belongs to the line before it
            if (tag_len && !(g->secret[0] && tag_len == strlen(g->secret) &&
                             strncmp(p + strspn(p, " \n"), g->secret, tag_len) == 0)) {
                keep = p;
            }
        }
    }
    if (!keep) {
        return;
    }
    if (keep != text) {
        ESP_LOGW(TAG, "the model thought aloud: only its last tagged line kept");
        memmove(text, keep, strlen(keep) + 1);
    }
    char *end = strstr(text, "\n\n");
    if (end) {
        *end = '\0';
    }
}

/*
 * sentences: the line cut after that many sentences (asked one short sentence, Claude made three of 40 words: the
 * quick-fire was lost). A sentence ends with . ! or ? (several in a row: one end), then a space.
 */
static void line_sentences(char *text, int max)
{
    if (max <= 0) {
        return;
    }
    size_t tag_len = 0;
    char *p = (char *)game_tag_skip(text, &tag_len);
    int count = 0;
    for (; *p; p++) {
        if (*p != '.' && *p != '!' && *p != '?') {
            continue;
        }
        while (p[1] == '.' || p[1] == '!' || p[1] == '?') {
            p++;
        }
        if (p[1] == ' ' || p[1] == '\n') {
            if (++count >= max) {
                ESP_LOGI(TAG, "line cut after %d sentence(s)", max);
                p[1] = '\0';
                return;
            }
        }
    }
}

/* A line said: the claims of the next one */
static void electing_start(uint64_t speaker)
{
    s_last_speaker = speaker;
    s_speaker = 0;
    s_state = G_ELECTING;
    s_done_us = esp_timer_get_time();
    s_since = s_done_us;
    s_reclaim_us = 0;
}

/*
 * My line: "speak" (the others start listening), shown, spoken, then "done" with its words (used only by a BOX that
 * heard nothing). Its tag is sent, never spoken; secret: "ANSWER: ..." kept for itself (in its history).
 */
static void line_say(char *text)
{
    int turn = s_turns + 1;
    ESP_LOGI(TAG, "line %d (mine): %s", turn, text);
    char *open = strdup(text);
    if (!open) {
        return;
    }
    const char *secret = game_def(s_game)->secret;
    if (secret[0]) {
        for (char *p = strstr(open, secret); p; p = strstr(p + 1, secret)) {
            const char *after = p + strlen(secret);
            after += strspn(after, " ");
            if (*after == ':') {
                // The answer remembered: not used again in the next games
                char answer[100];
                after++;
                after += strspn(after, " ");
                snprintf(answer, sizeof(answer), "%.*s", (int)strcspn(after, "\n"), after);
                for (size_t n = strlen(answer); n && strchr(" .!", answer[n - 1]); n--) {
                    answer[n - 1] = '\0';
                }
                if (answer[0]) {
                    game_used_add(game_def(s_game)->id, answer);
                }
                *p = '\0';
                break;
            }
        }
        for (size_t n = strlen(open); n && (open[n - 1] == ' ' || open[n - 1] == '\n' || open[n - 1] == '('); n--) {
            open[n - 1] = '\0';
        }
    }
    char tag[16];
    game_tag_of(text, tag, sizeof(tag));
    cJSON *m = msg_new("speak");
    cJSON_AddNumberToObject(m, "turn", turn);
    cJSON_AddStringToObject(m, "tag", tag);
    send_all(m);
    size_t tag_len;
    const char *said = game_tag_skip(open, &tag_len);
    ui_ctrl_reply_show_scrolled(said);
    s_spoken = false;
    tts_stream_begin(on_speech);
    tts_stream_say(said);
    tts_stream_end();
    int64_t t0 = esp_timer_get_time();
    while (!s_spoken && !s_stop_requested && esp_timer_get_time() - t0 < 60 * 1000000LL) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (tts_stream_moderated() && !s_stop_requested) {
        // The speech service refused the line (Mistral's moderation, an insult): written under it, and said once
        // a game (that sentence passes) so that the user knows why this BOX was silent
        ESP_LOGW(TAG, "line %d refused by the moderation of the speech service", turn);
        ui_ctrl_reply_append_notice(app_lang_get()->game_voice_refused);
        if (!s_refusal_said) {
            s_refusal_said = true;
            s_spoken = false;
            tts_stream_begin(on_speech);
            tts_stream_say(app_lang_get()->game_voice_refused);
            tts_stream_end();
            t0 = esp_timer_get_time();
            while (!s_spoken && !s_stop_requested && esp_timer_get_time() - t0 < 30 * 1000000LL) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
    }
    if (!s_stop_requested) {
        cJSON *d = msg_new("done");
        cJSON_AddNumberToObject(d, "turn", turn);
        cJSON_AddStringToObject(d, "tag", tag);
        cJSON_AddStringToObject(d, "text", said);
        send_all(d);
    }
    free(open);
    history_add(s_my_id, tag, text);
    s_turns = turn;
    electing_start(s_my_id);
}

/* The line of another BOX listened to (and its "done" received): kept, then this BOX claims the next one */
static int words_of(const char *text)
{
    int n = 0;
    for (const char *p = text; *p;) {
        p += strspn(p, " ,.;:!?'\n");
        size_t len = strcspn(p, " ,.;:!?'\n");
        n += len > 0;
        p += len;
    }
    return n;
}

static void line_heard(void)
{
    // Heard, unless the listening caught much less than the words sent ("S" for "Est-ce Marie Curie ?")
    bool heard = s_heard && s_heard[0] &&
                 (!s_done_text || words_of(s_heard) * 2 >= words_of(s_done_text));
    const char *text = heard ? s_heard : s_done_text ? s_done_text : "";
    ESP_LOGI(TAG, "line %d (player %d, %s): %s%s", s_turns + 1, player_number(s_speaker), s_line_tag,
             text, heard ? "" : s_heard && s_heard[0] ? " (heard only a part: the words sent)"
             : " (not heard: the words sent)");
    char *line = NULL;
    asprintf(&line, "%s%s%s", s_line_tag, s_line_tag[0] ? ": " : "", text);
    history_add(s_speaker, s_line_tag, line ? line : text);
    free(line);
    free(s_heard);
    s_heard = NULL;
    free(s_done_text);
    s_done_text = NULL;
    s_turns++;
    electing_start(s_speaker);
    claim_send(0);
}

/*
 * document: at the end, the game written down by the BOX ending it (the creation: the requirements for a
 * subcontractor, PROJECT.TXT on the configuration drive, replaced each time). Allocated: what to say after it, NULL
 * if no document
 */
static char *document_write(void)
{
    const game_def_t *g = game_def(s_game);
    if (!g->document[0] || !g->document_rule || cJSON_GetArraySize(s_history) == 0) {
        return NULL;
    }
    assistant_show(app_lang_get()->game_document_writing);
    char *system = NULL;
    asprintf(&system, "You write clear documents in the language whose code is '%s', from a discussion held out loud "
             "by several voice assistants (its words transcribed, some of them misheard).", app_lang_get()->code);
    char *question = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&question, &size);
    if (out) {
        fprintf(out, "Game: %s. The discussion, oldest first:\n", g->name);
        const cJSON *item;
        cJSON_ArrayForEach(item, s_history) {
            const cJSON *id = cJSON_GetObjectItem(item, "id");
            int number = cJSON_IsString(id) ? player_number(strtoull(id->valuestring, NULL, 16)) : 0;
            fprintf(out, "Player %d: %s\n", number, cJSON_GetObjectItem(item, "text")->valuestring);
        }
        fprintf(out, "\n%s", g->document_rule);
        fclose(out);
    }
    char *answer = NULL;
    llm_error_t err = system && question ? llm_ask_apart(system, question, &answer, g->reasoning) : LLM_ERR_OTHER;
    free(system);
    free(question);
    char path[48], name[24];
    snprintf(path, sizeof(path), "/spiffs/%s", g->document);
    snprintf(name, sizeof(name), "%s", g->document);
    for (char *c = name; *c; c++) {
        *c = (char)toupper((unsigned char)*c);
    }
    bool ok = false;
    if (err == LLM_OK && answer && answer[0]) {
        FILE *f = fopen(path, "w");
        ok = f && fputs(answer, f) >= 0;
        if (f) {
            ok &= fclose(f) == 0;
        }
    }
    ESP_LOGI(TAG, "%s: %s (%u characters)", path, ok ? "written" : "not written", answer ? (unsigned)strlen(answer) : 0);
    free(answer);
    char *said = NULL;
    if (ok) {
        asprintf(&said, app_lang_get()->game_document_written, name);
    }
    return said;
}

/* Elected: speak (the next line, or the end after the last one), else wait for the one elected */
static void decided(uint64_t winner)
{
    ESP_LOGI(TAG, "line %d: player %d%s", s_turns + 1, player_number(winner), winner == s_my_id ? " (this BOX)" : "");
    s_reclaim_us = 0;
    s_since = esp_timer_get_time();
    if (s_turns >= s_turns_max) {
        if (winner == s_my_id) {
            send_simple("stop", s_turns, NULL);
            char *document = document_write();
            char *said = NULL;
            asprintf(&said, "%s%s%s", app_lang_get()->game_end_natural, document ? " " : "", document ? document : "");
            game_end(said ? said : app_lang_get()->game_end_natural);
            free(said);
            free(document);
        } else {
            s_state = G_LISTENING;      // its "stop" ends this one quietly
        }
        return;
    }
    s_state = winner == s_my_id ? G_SPEAKING : G_LISTENING;
    if (winner != s_my_id) {
        // Another one speaks next: this one shows it listens at once (its listening starts with the voice)
        ui_ctrl_show_panel(UI_CTRL_PANEL_LISTEN, 0);
    }
}

/*
 * The claims of the line after s_turns: the earliest of the latest round wins; a tie: the BOX tied claim again
 * after a random delay. Decided once every BOX that listened claimed, or a while after the first claim.
 */
static void elect(void)
{
    int64_t now = esp_timer_get_time();
    int round = -1, claims = 0, expected = 0;
    int64_t first_us = 0;
    for (int i = 0; i < s_player_count; i++) {
        player_t *p = &s_players[i];
        expected += p->id != s_last_speaker;
        if (p->claim_turn == s_turns) {
            claims++;
            round = p->round > round ? p->round : round;
            first_us = !first_us || p->seen_us < first_us ? p->seen_us : first_us;
        }
    }
    bool start = s_state == G_STARTING;
    if (s_reclaim_us && now >= s_reclaim_us) {
        s_reclaim_us = 0;
        claim_send(round + 1);
        return;
    }
    if (start) {
        // The claims a moment; alone, the others a while longer (a BOX that heard it may transcribe it later)
        if (!claims || now - first_us < GAME_START_WAIT_MS * 1000LL ||
                (s_player_count < 2 && now - first_us < GAME_INVITE_MS * 1000LL)) {
            return;
        }
    } else if (claims < expected && (!claims || now - first_us < GAME_HEARD_WAIT_MS * 1000LL)) {
        if (!claims && now - s_done_us > GAME_DONE_WAIT_MS * 1000LL) {
            // Nobody heard it: the lowest id but the last speaker goes on
            uint64_t low = 0;
            for (int i = 0; i < s_player_count; i++) {
                if (s_players[i].id != s_last_speaker && (!low || s_players[i].id < low)) {
                    low = s_players[i].id;
                }
            }
            if (low) {
                decided(low);
            } else {
                game_end(app_lang_get()->game_peer_left);
            }
        }
        return;
    }
    if (start && s_player_count < 2) {
        game_end(app_lang_get()->game_no_peer);
        return;
    }
    // A rule of the game names who answers this line's tag (the riddles: its asker judges a guess)
    const cJSON *last = cJSON_GetArrayItem(s_history, cJSON_GetArraySize(s_history) - 1);
    const cJSON *last_tag = last ? cJSON_GetObjectItem(last, "tag") : NULL;
    uint64_t owner = !start && cJSON_IsString(last_tag) ? owner_after(last_tag->valuestring) : 0;
    if (owner && owner != s_last_speaker && player_find(owner)) {
        decided(owner);
        return;
    }
    player_t *best = NULL;
    int tied = 0;
    for (int i = 0; i < s_player_count; i++) {
        player_t *p = &s_players[i];
        if (p->claim_turn != s_turns || p->round != round) {
            continue;
        }
        if (!best || p->ts < best->ts) {
            best = p;
            tied = 1;
        } else if (p->ts == best->ts) {
            tied++;
        }
    }
    if (!best) {
        return;
    }
    if (tied > 1) {
        // The same time: the BOX tied claim again, each after its own random delay
        player_t *me = player_find(s_my_id);
        if (me && me->claim_turn == s_turns && me->round == round && me->ts == best->ts && !s_reclaim_us) {
            s_reclaim_us = now + (100 + esp_random() % 400) * 1000LL;
            ESP_LOGI(TAG, "claims at the same time: claiming again");
        }
        return;
    }
    if (start) {
        // The game of the claim elected: its id, its number of lines
        s_nonce = best->nonce;
        if (best->turns_max > 0) {
            s_turns_max = best->turns_max;
        }
        char title[96];
        snprintf(title, sizeof(title), app_lang_get()->game_title, game_def(s_game)->name);
        assistant_show(title);
        ESP_LOGI(TAG, "%s: %d BOX, %s, %d lines at most", game_def(s_game)->id, s_player_count,
                 s_tone_text ? s_tone_text : "-", s_turns_max);
    }
    decided(best->id);
}

/* A message of the network task, or "local": the listening of this BOX ended */
static void message(game_msg_t *m)
{
    if (strcmp(m->type, "start") == 0) {
        game_start(m->turn);
    } else if (strcmp(m->type, "local") == 0) {
        s_listen_on = false;
        if (m->text && m->text[0]) {
            char *joined = NULL;
            asprintf(&joined, "%s%s%s", s_heard ? s_heard : "", s_heard ? " " : "", m->text);
            free(s_heard);
            s_heard = joined;
        }
        if (s_state == G_LISTENING && s_speaker) {
            if (!s_done_seen) {
                // The speaker paused 1.5 s but goes on: listened to again
                s_listen_more = true;
                s_listen_on = true;
                app_sr_start_game_listen(GAME_LISTEN_WAIT_MS);
            } else {
                line_heard();
            }
        }
    } else if (strcmp(m->type, "claim") == 0) {
        games_load();
        int game = game_find(m->game);
        if (game < 0) {
            // not a game of this BOX
        } else {
            if (m->tone >= 0 && m->tone < 32) {
                s_tones_seen |= 1u << m->tone;
            }
            if (s_state == G_IDLE && ui_ctrl_is_asleep()) {
                // Asleep: it takes part (it listens, claims the lines it hears)
                start_setup(game, m->nonce);
                claim_take(m);
                cJSON *j = msg_new("join");
                cJSON_AddNumberToObject(j, "tone", s_my_tone);
                send_all(j);
                ESP_LOGI(TAG, "%s: joined", game_def(game)->id);
            } else if (s_state == G_IDLE) {
                // Busy (the phrase heard here too, being transcribed): kept, claimed once transcribed
                s_claim_seen = *m;
                s_claim_seen.text = NULL;
                s_claim_seen_us = esp_timer_get_time();
                ESP_LOGI(TAG, "%s: claim received while busy, kept", m->game);
            } else if (s_state == G_STARTING && game == s_game) {
                claim_take(m);
            }
        }
    } else if (strcmp(m->type, "join") == 0) {
        if (m->tone >= 0 && m->tone < 32) {
            s_tones_seen |= 1u << m->tone;
        }
        if (s_state == G_STARTING) {
            player_add(m->from);
        }
    } else if (s_state != G_IDLE && s_state != G_STARTING && m->nonce == s_nonce) {
        player_add(m->from);
        if (strcmp(m->type, "heard") == 0) {
            claim_take(m);
        } else if (strcmp(m->type, "speak") == 0 && m->turn == s_turns + 1 && s_state != G_SPEAKING &&
                   !(s_state == G_LISTENING && s_speaker == m->from)) {
            // Another BOX says the next line: listened to with the microphone
            s_speaker = m->from;
            strlcpy(s_line_tag, m->tag, sizeof(s_line_tag));
            free(s_heard);
            s_heard = NULL;
            free(s_done_text);
            s_done_text = NULL;
            s_done_seen = false;
            s_listen_more = false;
            s_state = G_LISTENING;
            s_since = esp_timer_get_time();
            s_listen_on = true;
            app_sr_start_game_listen(GAME_LISTEN_WAIT_MS);
        } else if (strcmp(m->type, "done") == 0 && !s_speaker && m->turn == s_turns + 1 && s_state != G_SPEAKING) {
            // Its "speak" lost: not listened to, the words it sent are used
            ESP_LOGW(TAG, "line %d: its start was not received", m->turn);
            s_speaker = m->from;
            strlcpy(s_line_tag, m->tag, sizeof(s_line_tag));
            free(s_heard);
            s_heard = NULL;
            s_done_seen = true;
            s_done_us = esp_timer_get_time();
            free(s_done_text);
            s_done_text = m->text;
            m->text = NULL;
            s_state = G_LISTENING;
            line_heard();
        } else if (strcmp(m->type, "done") == 0 && m->from == s_speaker && m->turn == s_turns + 1) {
            s_done_seen = true;
            s_done_us = esp_timer_get_time();
            free(s_done_text);
            s_done_text = m->text;
            m->text = NULL;
            if (!s_listen_on) {
                line_heard();
            }
        } else if (strcmp(m->type, "stop") == 0) {
            // Quietly: the one stopping says the end (or listens to the user who stopped it)
            tts_stream_cancel();
            game_end(NULL);
        }
    } else if (s_state == G_STARTING && strcmp(m->type, "speak") == 0 && m->turn == 1) {
        // The first line already said by the BOX elected (this one claimed later): its game is joined
        s_nonce = m->nonce;
        s_state = G_ELECTING;
        message(m);
        return;
    }
    free(m->text);
}

/* A message logged (its copies are not): the turns of a game followed in the log */
static void message_trace(const game_msg_t *m)
{
    static char s_last[48];
    char key[48];
    snprintf(key, sizeof(key), "%s %llx %d %d %lu", m->type, (unsigned long long)m->from, m->turn, m->round,
             (unsigned long)m->nonce);
    if (strcmp(m->type, "local") == 0 || strcmp(key, s_last) != 0) {
        strlcpy(s_last, key, sizeof(s_last));
        ESP_LOGI(TAG, "received %s, line %d, from %s, game %s (state %d, line %d, speaker %s)", m->type, m->turn,
                 m->from == s_my_id ? "this BOX" : m->from == s_speaker ? "the speaker" : "another BOX",
                 m->nonce == s_nonce ? "this one" : "another", (int)s_state, s_turns + 1,
                 s_speaker ? "known" : "none");
    }
}

void game_poll(void)
{
    game_msg_t m;
    while (game_net_receive(&m)) {
        if (s_state != G_IDLE || strcmp(m.type, "claim") == 0) {
            message_trace(&m);
        }
        message(&m);
    }
    if (s_stop_requested) {
        s_stop_requested = false;
        if (s_state != G_IDLE && s_state != G_STARTING) {
            send_simple("stop", s_turns, NULL);
        }
        if (s_state != G_IDLE) {
            ESP_LOGI(TAG, "Stopped by the wake word or a touch");
            game_reset();
        }
        return;
    }
    int64_t now = esp_timer_get_time();
    switch (s_state) {
    case G_STARTING:
        elect();
        break;
    case G_ELECTING:
        elect();
        break;
    case G_LISTENING:
        if (s_speaker && s_listen_on && s_done_seen &&
                now - s_done_us > (s_listen_more ? GAME_MORE_WAIT_MS : GAME_TAIL_WAIT_MS) * 1000LL) {
            // Listened to again after a pause, but the speaker had finished (or a noise keeps the listening on):
            // what was heard is kept, else the words sent
            listen_stop();
            line_heard();
        } else if (!s_speaker && now - s_since > GAME_SPEAK_WAIT_MS * 1000LL) {
            // The one elected never spoke (or, after the last line, never ended the game)
            game_end(s_turns >= s_turns_max ? NULL : app_lang_get()->game_peer_left);
        } else if (s_speaker && !s_done_seen && now - s_since > GAME_SILENCE_MS * 1000LL) {
            game_end(app_lang_get()->game_peer_left);
        }
        break;
    case G_SPEAKING: {
        char *text = line_make();
        char allowed[32], tag[16];
        next_tags(allowed, sizeof(allowed));
        if (text && allowed[0]) {
            line_clean(text);
            game_tag_of(text, tag, sizeof(tag));
            if (!tag[0] || !game_tag_in(allowed, strlen(allowed), tag)) {
                ESP_LOGW(TAG, "line tagged \"%s\" instead of %s: asked again", tag, allowed);
                free(text);
                text = line_make();
            }
        }
        // A line that must give the secret (a new riddle, the person thought of) without it: the model has
        // nothing to answer from afterwards
        const game_def_t *g = game_def(s_game);
        if (text && g->secret[0] && g->secret_with[0]) {
            char secret[20];
            snprintf(secret, sizeof(secret), "%s:", g->secret);
            line_clean(text);
            game_tag_of(text, tag, sizeof(tag));
            if (tag[0] && game_tag_in(g->secret_with, strlen(g->secret_with), tag) && !strstr(text, secret)) {
                ESP_LOGW(TAG, "line tagged %s without %s: asked again", tag, secret);
                free(text);
                static char note[96];
                snprintf(note, sizeof(note), "Do not forget to end this line with %s followed by your answer.", secret);
                s_line_note = note;
                text = line_make();
                s_line_note = NULL;
            }
        }
        if (!text) {
            send_simple("stop", s_turns, NULL);
            game_end(app_lang_get()->game_peer_left);
            return;
        }
        line_clean(text);
        line_shorten(text, game_def(s_game)->short_after);
        line_sentences(text, game_def(s_game)->sentences);
        line_say(text);
        free(text);
        break;
    }
    default:
        break;
    }
}
