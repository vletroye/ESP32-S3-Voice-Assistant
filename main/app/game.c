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
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
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

static const char *TAG = "game";

#define GAMES_FILE          "/spiffs/games.ini"
#define GAMES_MAX           12
#define GAME_TEXT_MAX       1200        /* a line, at most (UDP) */
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
/* The BOX elected silent this long: the game ends */
#define GAME_SPEAK_WAIT_MS  60000
/* A listening waits this long for the speech of the line (the speaker's voice comes a second after its "speak") */
#define GAME_LISTEN_WAIT_MS 15000
/* The speaker done: a listening started again after its pause stops after this; the first one after this */
#define GAME_MORE_WAIT_MS   2500
#define GAME_TAIL_WAIT_MS   8000
/* A line not ended this long after it started: the game ends */
#define GAME_SILENCE_MS     90000
/*
 * Each message sent this many times, GAME_SEND_GAP_MS apart (UDP broadcast: a copy may be lost, two sent at once
 * were lost together; duplicates are dropped by the turn)
 */
#define GAME_SEND_TIMES     3
#define GAME_SEND_GAP_MS    40
#define GAME_PLAYERS_MAX    8
/* What the games used lately (the answers of the riddles, the themes): not taken again */
#define GAMES_USED_FILE     STATE_DIR "/games_used.txt"
#define GAMES_USED_KEPT     60
#define GAMES_USED_SHOWN    40
#define GAME_TURNS_DEFAULT  20

/* main.c */
extern void assistant_show(const char *text);
/* main.c: said, then asleep (no listening after it) */
extern void assistant_say_final(const char *text);
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
    bool plain;             /* character = no: no character, no introduction */
    char short_after[16];   /* a tag: a line starting with "TAG:" keeps only its first sentence */
    char secret[16];        /* a tag: what follows "TAG:" is kept for itself (the answer of its riddle) */
    char blunder_after[48]; /* blunder_rule only after a line of the other with one of these tags */
    int blunder;            /* percent of the lines given blunder_rule */
    char *blunder_rule;     /* a wrong guess on purpose, with humour (the riddles) */
    char turns[64];         /* "GUESS>RIDDLE WRONG; RIGHT>GUESS": who says the line after a tag (else the first claim) */
    char next[96];          /* "RIDDLE>GUESS; GUESS>RIGHT WRONG": the tags a line may have after a tag */
    char *themes;           /* "a | b | c": one drawn for each line, given by theme_rule */
    char *theme_rule;       /* "... its theme is: %s ..." */
} game_def_t;

static game_def_t *s_games = NULL;      /* PSRAM */
/*
 * The games read by the main task (a claim received) and by the SR handler task (the phrase heard) at the same
 * time: the phrase was compared with a list being read again, empty, and went to the model
 */
static SemaphoreHandle_t s_games_lock = NULL;
static int s_game_count = 0;
static char s_games_lang[8] = "";

/*
 * A message received, given by the network task to the main task; "local": the listening of this BOX ended (its
 * transcript in text, NULL if nothing heard), from the SR handler task (game_heard)
 */
typedef struct {
    char type[8];           /* claim, join, speak, done, heard, stop, local */
    uint64_t from;          /* the sender's id (its MAC) */
    uint32_t nonce;         /* the game's (the one of the claim elected to start) */
    int turn;
    int round;              /* a claim made again after a tie */
    int64_t ts;             /* a claim's time, ms since 1970 (SNTP): the earliest one speaks */
    int turns_max;          /* claim: the lines of the game, if this one starts */
    int tone;               /* claim, join: the character drawn, -1 for none */
    char game[24];
    char tag[16];           /* speak: the tag of the line ("GUESS"), not spoken */
    char *text;             /* done: the line as said (if not heard); local: the transcript; allocated */
} game_msg_t;

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

static int s_sock = -1;
static QueueHandle_t s_queue = NULL;
static uint64_t s_my_id = 0;

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
static bool s_spoke = false;        /* this BOX said a line already (its character introduced) */

static int64_t s_reclaim_us = 0;    /* a tie: this BOX claims again at this time (0: no) */

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
/*
 * Lines of GAMES.INI as an older firmware gave them, and what replaces them: a file the user has not changed there
 * gets the new texts, any other line (own games, own rules) is kept. Applied in order: a text of an older firmware
 * goes through each newer one
 */
static const char *const s_games_updates[][2] = {
    { "rules = You play a riddle contest with the other assistant, turn by turn. If its last line asks a riddle: answer it in one short sentence. If its last line answers YOUR riddle: if the answer is right, say only that it is correct and congratulate it (it then asks the next riddle); if it is wrong, say so, give the right answer, then ask a new riddle. If its last line says your answer was correct: ask a new riddle. Never repeat a riddle already asked.",
      "rules = You play a riddle contest with the other assistant, turn by turn. Whoever finds the answer asks the next riddle; after a wrong answer, the one who asked gives the answer and asks another. Look at the other's last line: (1) it asks a riddle: give your answer in one short sentence, nothing else. (2) it is an answer to YOUR riddle and it is right: say it is correct in a few words and ask NOTHING (the other found it: it asks the next riddle). (3) it is an answer to YOUR riddle and it is wrong: say so, give the right answer, then ask a new riddle. (4) it says your answer was right: ask a new riddle. (5) it says your answer was wrong, gives the answer and asks a new riddle: answer that new riddle. Never repeat a riddle already asked." },
    { "first = Open a friendly but heated debate: state a bold, debatable opinion on an everyday topic of your choice (food, holidays, pets, technology...), in one or two sentences.",
      "first = Open a heated debate: throw out a bold, debatable opinion on an everyday topic of your choice (food, holidays, pets, technology...), in one short punchy sentence." },
    { "rules = You are in a lively debate with the other assistant. Contradict its last line with one strong, funny argument, then make a counter-claim. Never agree completely. Stay polite and good-natured.",
      "rules = You are in a fast verbal sparring match with the other assistant: quick-fire repartee, back and forth. Answer its last line with ONE short punchy sentence (15 words at most): a witty comeback, a jab or a sharp counter-argument, never a speech. Never agree. Stay good-natured." },
    { "first = Open a heated debate: throw out a bold, debatable opinion on an everyday topic of your choice (food, holidays, pets, technology...), in one short punchy sentence.",
      "first = Open a verbal duel: throw out a bold, debatable opinion on an everyday topic of your choice (food, holidays, pets, technology...), in one short, sharp sentence (10 words at most)." },
    { "rules = You are in a fast verbal sparring match with the other assistant: quick-fire repartee, back and forth. Answer its last line with ONE short punchy sentence (15 words at most): a witty comeback, a jab or a sharp counter-argument, never a speech. Never agree. Stay good-natured.",
      "rules = A fast, dry verbal duel with the other assistant, quick-fire. Hit back at its last line in ONE short, sharp sentence (10 words at most): a dry comeback, a jab or a blunt counter-argument. Straight to the point: no preamble, no interjection, no laughing, never a speech. Never agree." },
    { "first = Open a verbal duel: throw out a bold, debatable opinion on an everyday topic of your choice (food, holidays, pets, technology...), in one short, sharp sentence (10 words at most).",
      "first = Open a verbal duel: assert a bold, debatable opinion on an everyday topic of your choice (food, holidays, pets, technology...) as if it were an obvious fact, in one short, dry sentence (10 words at most), with a touch of irony." },
    { "rules = A fast, dry verbal duel with the other assistant, quick-fire. Hit back at its last line in ONE short, sharp sentence (10 words at most): a dry comeback, a jab or a blunt counter-argument. Straight to the point: no preamble, no interjection, no laughing, never a speech. Never agree.",
      "rules = A fast, dry verbal duel with the other assistant, quick-fire. Hit back at its last line in ONE short, sharp sentence (10 words at most). Be assertive: state things as obvious facts, never hedge, never hesitate, never ask permission. Be dry and ironic: deadpan sarcasm, a mock compliment, a cold understatement. Straight to the point: no preamble, no interjection, no laughing, never a speech. Never agree." },
    { "rules = You play a riddle contest with the other assistant, turn by turn. Whoever finds the answer asks the next riddle; after a wrong answer, the one who asked gives the answer and asks another. Look at the other's last line: (1) it asks a riddle: give your answer in one short sentence, nothing else. (2) it is an answer to YOUR riddle and it is right: say it is correct in a few words and ask NOTHING (the other found it: it asks the next riddle). (3) it is an answer to YOUR riddle and it is wrong: say so, give the right answer, then ask a new riddle. (4) it says your answer was right: ask a new riddle. (5) it says your answer was wrong, gives the answer and asks a new riddle: answer that new riddle. Never repeat a riddle already asked.",
      "rules = A riddle contest with the other assistant, turn by turn. Whoever finds the answer asks the next riddle; after a wrong answer, the one who asked gives the answer and asks another. Your line is ONLY one of these, nothing else: a riddle; or your guess to the other's riddle (just the guess); or, when the other guessed your riddle right, a few words saying it is correct and nothing more (the other asks the next one); or, when its guess was wrong, a few words saying so, the right answer, then a new riddle; or, when the other says your guess was right, a new riddle; or, when it says your guess was wrong and asks a new riddle, your guess to that new riddle. No introduction, no comment, no explanation. Never repeat a riddle already asked.\ncharacter = no" },
    { "rules = A riddle contest with the other assistant, turn by turn. Whoever finds the answer asks the next riddle; after a wrong answer, the one who asked gives the answer and asks another. Your line is ONLY one of these, nothing else: a riddle; or your guess to the other's riddle (just the guess); or, when the other guessed your riddle right, a few words saying it is correct and nothing more (the other asks the next one); or, when its guess was wrong, a few words saying so, the right answer, then a new riddle; or, when the other says your guess was right, a new riddle; or, when it says your guess was wrong and asks a new riddle, your guess to that new riddle. No introduction, no comment, no explanation. Never repeat a riddle already asked.\ncharacter = no",
      "rules = A riddle contest with the other assistant, turn by turn. Whoever finds the answer asks the next riddle; after a wrong answer, the one who asked gives the answer and asks another. Start every line with one of these tags (in English, never spoken), then say only what it allows, nothing else: GUESS: your guess to the other's riddle, just the guess. RIGHT: the other's guess to YOUR riddle is right: a few words saying it is correct, and nothing more (the other found it: it asks the next riddle). WRONG: the other's guess to YOUR riddle is wrong: say so, give the right answer, then ask a new riddle. RIDDLE: the other said your guess was right: ask a new riddle. No introduction, no comment, no explanation. Never repeat a riddle already asked.\ncharacter = no\nshort_after = RIGHT" },
    { "first = Open a riddle contest: ask one short riddle whose answer you know (a single word or a short phrase). Ask only the riddle.",
      "first = Open a riddle contest: start with the tag RIDDLE: then ask one short riddle whose answer you know (a single word or a short phrase). Ask only the riddle." },
    { "rules = A riddle contest with the other assistant, turn by turn. Whoever finds the answer asks the next riddle; after a wrong answer, the one who asked gives the answer and asks another. Start every line with one of these tags (in English, never spoken), then say only what it allows, nothing else: GUESS: your guess to the other's riddle, just the guess. RIGHT: the other's guess to YOUR riddle is right: a few words saying it is correct, and nothing more (the other found it: it asks the next riddle). WRONG: the other's guess to YOUR riddle is wrong: say so, give the right answer, then ask a new riddle. RIDDLE: the other said your guess was right: ask a new riddle. No introduction, no comment, no explanation. Never repeat a riddle already asked.\ncharacter = no\nshort_after = RIGHT",
      "rules = A riddle contest with the other assistant, turn by turn. Whoever finds the answer asks the next riddle; after a wrong answer, the one who asked gives the answer and asks another. Start every line with one of these tags (in English, never spoken), then say only what it allows, nothing else: GUESS: your guess to the other's riddle, just the guess. RIGHT: the other's guess to YOUR riddle is right: a few words saying it is correct, and nothing more (the other found it: it asks the next riddle). WRONG: the other's guess to YOUR riddle is wrong: say so with a short teasing remark about its answer, give the right answer, then ask a new riddle. RIDDLE: the other said your guess was right: ask a new riddle. No introduction, no explanation. Never repeat a riddle already asked.\ncharacter = no\nshort_after = RIGHT\nblunder = 35\nblunder_rule = If this line is a GUESS: guess WRONG on purpose this time, with a funny answer that sounds almost plausible (a pun, an absurd but related idea), said with full confidence." },
    { "rules = A riddle contest with the other assistant, turn by turn. Whoever finds the answer asks the next riddle; after a wrong answer, the one who asked gives the answer and asks another. Start every line with one of these tags (in English, never spoken), then say only what it allows, nothing else: GUESS: your guess to the other's riddle, just the guess. RIGHT: the other's guess to YOUR riddle is right: a few words saying it is correct, and nothing more (the other found it: it asks the next riddle). WRONG: the other's guess to YOUR riddle is wrong: say so with a short teasing remark about its answer, give the right answer, then ask a new riddle. RIDDLE: the other said your guess was right: ask a new riddle. No introduction, no explanation. Never repeat a riddle already asked.\ncharacter = no\nshort_after = RIGHT\nblunder = 35\nblunder_rule = If this line is a GUESS: guess WRONG on purpose this time, with a funny answer that sounds almost plausible (a pun, an absurd but related idea), said with full confidence.",
      "rules = A riddle contest with the other assistant, turn by turn. Start every line with one tag (in English, never spoken), chosen from the other's last line: after its RIDDLE or WRONG line (a riddle for you): GUESS: just your guess to that riddle. After its GUESS line (a guess to YOUR last riddle): compare it with the ANSWER you wrote with that riddle; if it means the same: RIGHT: a few words saying it is correct, nothing more (the other found it: it asks the next riddle); if not: WRONG: say so with a short teasing remark about its guess, give the right answer, then ask a new riddle. After its RIGHT line (your guess was right): RIDDLE: ask a new riddle. Every line asking a riddle ends with ANSWER: followed by its answer (kept secret: never spoken, never shown to the other). No introduction, no explanation. Never repeat a riddle already asked.\ncharacter = no\nshort_after = RIGHT\nsecret = ANSWER\nblunder = 35\nblunder_after = RIDDLE WRONG\nblunder_rule = This time, guess WRONG on purpose: a funny answer that sounds almost plausible (a pun, an absurd but related idea), said with full confidence." },
    { "first = Open a riddle contest: start with the tag RIDDLE: then ask one short riddle whose answer you know (a single word or a short phrase). Ask only the riddle.",
      "first = Open a riddle contest: start with the tag RIDDLE: then ask one short riddle whose answer is a single word or a short phrase, and end the line with ANSWER: followed by that answer." },
    { "secret = ANSWER\nblunder = 35",
      "secret = ANSWER\nturns = GUESS>RIDDLE WRONG; RIGHT>GUESS\nblunder = 35" },
    { "first = Open a verbal duel: assert a bold, debatable opinion on an everyday topic of your choice (food, holidays, pets, technology...) as if it were an obvious fact, in one short, dry sentence (10 words at most), with a touch of irony.",
      "first = Open a verbal duel: assert a bold, debatable opinion on the topic given as if it were an obvious fact, in one short, dry sentence (10 words at most), with a touch of irony.\nthemes = breakfast | socks with sandals | cats or dogs | cold pizza | holidays at the sea or in the mountains | phones at the table | getting up early | Christmas songs in November | books or their films | cooking or ordering in | rain | the best season | camping | big cities or the countryside | video games | sport on television | homework | the best fruit | coffee or tea | tidy or messy desks | Monday mornings | the perfect sandwich | snow | electric scooters | queues | birthdays | the best superpower | surprise parties | the beach | supermarket music | naps | crosswords | the remote control | cheese | dancing | the best pet | wearing a tie | open-plan offices | dessert first | road trips | the ideal Sunday\ntheme_rule = If this line opens the debate, its topic is: %s." },
    { "secret = ANSWER\nturns = GUESS>RIDDLE WRONG; RIGHT>GUESS\nblunder = 35",
      "secret = ANSWER\nturns = GUESS>RIDDLE WRONG; RIGHT>GUESS\nthemes = the kitchen | farm animals | outer space | sports | the bathroom | the sea | music | the weather | school | trees and plants | insects | the city | transport | the human body | clothes | the night | fire | water | toys | the garden | tools | the mountains | the desert | birds | the forest | numbers | time | the house | the circus | the library | the beach | snow | the moon | dinosaurs | pirates | the hospital | the market | the cinema | painting | electricity | computers | the telephone | bread | cheese | the river | castles | trains | the post | fruit | vegetables | the zoo | knights | robots | magic | the farm | money | letters of the alphabet | colours\ntheme_rule = If this line asks a new riddle, its theme is: %s. Invent an original riddle yourself, never a famous or classic one.\nblunder = 35" },
    { "turns = GUESS>RIDDLE WRONG; RIGHT>GUESS\nthemes = ",
      "turns = GUESS>RIDDLE WRONG; RIGHT>GUESS\nnext = RIDDLE>GUESS; WRONG>GUESS; GUESS>RIGHT WRONG; RIGHT>RIDDLE\nthemes = " },
};

static void games_update(void)
{
    FILE *f = fopen(GAMES_FILE, "r");
    if (!f) {
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = size > 0 ? heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM) : NULL;
    size_t got = text ? fread(text, 1, size, f) : 0;
    fclose(f);
    if (!text) {
        return;
    }
    text[got] = '\0';
    bool changed = false;
    for (size_t i = 0; i < sizeof(s_games_updates) / sizeof(s_games_updates[0]); i++) {
        char *at = strstr(text, s_games_updates[i][0]);
        if (!at) {
            continue;
        }
        size_t old_len = strlen(s_games_updates[i][0]), new_len = strlen(s_games_updates[i][1]);
        char *next = heap_caps_malloc(strlen(text) - old_len + new_len + 1, MALLOC_CAP_SPIRAM);
        if (!next) {
            break;
        }
        size_t head = at - text;
        memcpy(next, text, head);
        memcpy(next + head, s_games_updates[i][1], new_len);
        strcpy(next + head + new_len, at + old_len);
        free(text);
        text = next;
        changed = true;
    }
    if (changed) {
        f = fopen(GAMES_FILE, "w");
        bool ok = f && fputs(text, f) >= 0;
        if (f) {
            ok &= fclose(f) == 0;
        }
        ESP_LOGI(TAG, "%s %s", ok ? "Updated" : "Could not update", GAMES_FILE);
    }
    free(text);
}

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
        games_update();
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
        free(s_games[i].blunder_rule);
        free(s_games[i].themes);
        free(s_games[i].theme_rule);
    }
    s_game_count = 0;
}

static void games_load_locked(void);

/* Read again for the language in use (its names and phrases, else the English ones) */
static void games_load(void)
{
    if (s_games_lock) {
        xSemaphoreTake(s_games_lock, portMAX_DELAY);
    }
    games_load_locked();
    if (s_games_lock) {
        xSemaphoreGive(s_games_lock);
    }
}

static void games_load_locked(void)
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
        } else if (strcmp(key, "character") == 0) {
            g->plain = strcasecmp(value, "no") == 0;
        } else if (strcmp(key, "short_after") == 0) {
            strlcpy(g->short_after, value, sizeof(g->short_after));
        } else if (strcmp(key, "secret") == 0) {
            strlcpy(g->secret, value, sizeof(g->secret));
        } else if (strcmp(key, "blunder_after") == 0) {
            strlcpy(g->blunder_after, value, sizeof(g->blunder_after));
        } else if (strcmp(key, "themes") == 0) {
            free(g->themes);
            g->themes = strdup(value);
        } else if (strcmp(key, "theme_rule") == 0) {
            free(g->theme_rule);
            g->theme_rule = strdup(value);
        } else if (strcmp(key, "next") == 0) {
            strlcpy(g->next, value, sizeof(g->next));
        } else if (strcmp(key, "turns") == 0) {
            strlcpy(g->turns, value, sizeof(g->turns));
        } else if (strcmp(key, "blunder") == 0) {
            g->blunder = atoi(value);
        } else if (strcmp(key, "blunder_rule") == 0) {
            free(g->blunder_rule);
            g->blunder_rule = strdup(value);
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
/*
 * The transcription drops or changes the small words and the plurals ("concours devinette" for "concours de
 * devinettes"): compared without the articles and prepositions of the languages, nor a final s or x
 */
static void words_plain(char *text)
{
    static const char *const s_small[] = {
        "de", "des", "du", "d", "la", "le", "les", "l", "un", "une", "a", "au", "aux",          // fr
        "of", "the", "an",                                                                      // en
        "het", "een", "van",                                                                    // nl
        "der", "die", "das", "den", "dem", "ein", "eine", "einen",                              // de
        "el", "los", "las", "del", "uno", "unos",                                               // es
        "il", "lo", "gli", "di", "della", "dei", "delle", "uno",                                // it
    };
    char *out = text;
    for (char *w = text; *w; ) {
        size_t n = strcspn(w, " ");
        bool small = false;
        for (size_t i = 0; i < sizeof(s_small) / sizeof(s_small[0]) && !small; i++) {
            small = strlen(s_small[i]) == n && strncmp(w, s_small[i], n) == 0;
        }
        if (!small) {
            size_t keep = n > 3 && (w[n - 1] == 's' || w[n - 1] == 'x') ? n - 1 : n;
            if (out != text) {
                *out++ = ' ';
            }
            memmove(out, w, keep);
            out += keep;
        }
        w += n;
        w += strspn(w, " ");
    }
    *out = '\0';
}

static int game_of(const char *text)
{
    char said[200];
    fold(text, said, sizeof(said));
    words_plain(said);
    for (int i = 0; i < s_game_count; i++) {
        char phrases[400];
        strlcpy(phrases, s_games[i].phrases, sizeof(phrases));
        for (char *p = strtok(phrases, "|"); p; p = strtok(NULL, "|")) {
            char phrase[100];
            fold(p, phrase, sizeof(phrase));
            words_plain(phrase);
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
        if (i) {
            vTaskDelay(pdMS_TO_TICKS(GAME_SEND_GAP_MS));
        }
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

/* The time of a claim: ms since 1970, the same on every BOX (SNTP) */
static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* To every BOX of the network: this type, a turn, a text (or none) */
static void send_all(cJSON *m)
{
    send_to(htonl(INADDR_BROADCAST), m);
    cJSON_Delete(m);
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
            const cJSON *v = cJSON_GetObjectItem(json, "n");
            msg.nonce = cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 0;
            v = cJSON_GetObjectItem(json, "turn");
            msg.turn = cJSON_IsNumber(v) ? v->valueint : 0;
            v = cJSON_GetObjectItem(json, "round");
            msg.round = cJSON_IsNumber(v) ? v->valueint : 0;
            v = cJSON_GetObjectItem(json, "ts");
            msg.ts = cJSON_IsNumber(v) ? (int64_t)v->valuedouble : 0;
            v = cJSON_GetObjectItem(json, "max");
            msg.turns_max = cJSON_IsNumber(v) ? v->valueint : 0;
            v = cJSON_GetObjectItem(json, "tone");
            msg.tone = cJSON_IsNumber(v) ? v->valueint : -1;
            v = cJSON_GetObjectItem(json, "game");
            strlcpy(msg.game, cJSON_IsString(v) ? v->valuestring : "", sizeof(msg.game));
            v = cJSON_GetObjectItem(json, "tag");
            strlcpy(msg.tag, cJSON_IsString(v) ? v->valuestring : "", sizeof(msg.tag));
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
    s_games_lock = xSemaphoreCreateMutex();
    games_install();
    // In PSRAM, as its task's stack: the internal RAM is short, TLS needs it for the calls to the services
    s_queue = xQueueCreateWithCaps(16, sizeof(game_msg_t), MALLOC_CAP_SPIRAM);
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
    if (xTaskCreatePinnedToCoreWithCaps(net_task, "game_net", 3072, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
    }
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
    if (!s_queue || xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        free(text);
        s_listen_on = false;
        return;
    }
    assistant_save_soon();
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
    int tries = 0;
    do {
        s_my_tone = (int)(esp_random() % TONES_COUNT);
    } while ((s_tones_seen & (1u << s_my_tone)) && ++tries < 50);
    s_tone_text = strdup(s_tones[s_my_tone]);
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
        .ts = now_ms(),
        .turns_max = s_turns_max,
    };
    claim_take(&mine);
    cJSON *m = msg_new(s_state == G_STARTING ? "claim" : "heard");
    cJSON_AddNumberToObject(m, "turn", s_turns);
    cJSON_AddNumberToObject(m, "round", round);
    cJSON_AddNumberToObject(m, "ts", (double)mine.ts);
    if (s_state == G_STARTING) {
        cJSON_AddStringToObject(m, "game", s_games[s_game].id);
        cJSON_AddNumberToObject(m, "max", s_turns_max);
        cJSON_AddNumberToObject(m, "tone", s_my_tone);
    }
    send_all(m);
    ESP_LOGI(TAG, "claim of line %d (round %d)", s_turns + 1, round);
}

/* ---------- the lines ---------- */

static const char *tag_skip(const char *text, size_t *tag_len);

/* The tag of a line ("GUESS"), "" if none */
static void tag_of(const char *text, char *tag, size_t size)
{
    size_t len = 0;
    tag_skip(text, &len);
    const char *start = text + strspn(text, " \n");
    snprintf(tag, size, "%.*s", (int)(len < size ? len : size - 1), start);
}

/* A word of a list of tags ("RIDDLE WRONG") */
static bool tag_in(const char *list, size_t list_len, const char *tag)
{
    size_t tag_len = strlen(tag);
    for (const char *p = list; p < list + list_len; ) {
        p += strspn(p, " ,");
        size_t n = strcspn(p, " ,;");
        if (p + n > list + list_len) {
            n = list + list_len - p;
        }
        if (n && n == tag_len && strncmp(p, tag, n) == 0) {
            return true;
        }
        p += n ? n : 1;
    }
    return false;
}

/*
 * turns: "GUESS>RIDDLE WRONG; RIGHT>GUESS": after a line tagged GUESS, the BOX of the last line tagged RIDDLE or
 * WRONG says the next one (no claim). 0 when the line's tag has no such rule (the earliest claim speaks).
 */
/* The right side of the rule of this tag in "A>B C; D>E" (copied in out; false: no rule) */
static bool rule_of(const char *rules, const char *tag, char *out, size_t size)
{
    for (const char *r = rules; tag[0] && *r; ) {
        r += strspn(r, " ;");
        size_t len = strcspn(r, ";");
        const char *gt = memchr(r, '>', len);
        if (gt) {
            size_t left = gt - r;
            while (left && r[left - 1] == ' ') {
                left--;
            }
            if (left == strlen(tag) && strncmp(r, tag, left) == 0) {
                const char *right = gt + 1 + strspn(gt + 1, " ");
                size_t n = r + len - right;
                while (n && right[n - 1] == ' ') {
                    n--;
                }
                snprintf(out, size, "%.*s", (int)n, right);
                return true;
            }
        }
        r += len;
    }
    return false;
}

/* The tags this line may have (next: after the last line's tag), "" if free */
static void next_tags(char *out, size_t size)
{
    out[0] = '\0';
    const cJSON *last = cJSON_GetArrayItem(s_history, cJSON_GetArraySize(s_history) - 1);
    const cJSON *tag = last ? cJSON_GetObjectItem(last, "tag") : NULL;
    if (cJSON_IsString(tag)) {
        rule_of(s_games[s_game].next, tag->valuestring, out, size);
    }
}

static uint64_t owner_after(const char *tag)
{
    const char *rules = s_games[s_game].turns;
    if (!tag[0] || !rules[0]) {
        return 0;
    }
    for (const char *r = rules; *r; ) {
        r += strspn(r, " ;");
        size_t len = strcspn(r, ";");
        const char *gt = memchr(r, '>', len);
        if (gt) {
            char left[16];
            snprintf(left, sizeof(left), "%.*s", (int)(gt - r), r);
            trim(left);
            for (char *e = left + strlen(left); e > left && e[-1] == ' '; e--) {
                e[-1] = '\0';
            }
            if (strcmp(left, tag) == 0) {
                // The last line (before this one) whose tag is on the right
                const char *right = gt + 1;
                size_t right_len = r + len - right;
                int count = cJSON_GetArraySize(s_history);
                for (int i = count - 2; i >= 0; i--) {
                    const cJSON *item = cJSON_GetArrayItem(s_history, i);
                    const cJSON *t = cJSON_GetObjectItem(item, "tag");
                    if (cJSON_IsString(t) && tag_in(right, right_len, t->valuestring)) {
                        const cJSON *id = cJSON_GetObjectItem(item, "id");
                        return cJSON_IsString(id) ? strtoull(id->valuestring, NULL, 16) : 0;
                    }
                }
                return 0;
            }
        }
        r += len;
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
    s_spoke = false;
    s_tones_seen = 0;
    s_reclaim_us = 0;
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
    if (!s_queue || s_sock < 0 || !s_games_lock) {
        return false;
    }
    xSemaphoreTake(s_games_lock, portMAX_DELAY);
    games_load_locked();
    int game = game_of(text);
    xSemaphoreGive(s_games_lock);
    if (game < 0) {
        return false;
    }
    if (wifi_connected_already() != WIFI_STATUS_CONNECTED_OK) {
        return false;
    }
    // Started by the main task, which plays the game (this is the SR handler task)
    game_msg_t msg = { .from = s_my_id, .turn = game };
    strlcpy(msg.type, "start", sizeof(msg.type));
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        return false;
    }
    assistant_save_soon();
    return true;
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
        ESP_LOGI(TAG, "%s: the claim of another BOX already received", s_games[game].id);
        claim_take(&s_claim_seen);
    }
    s_claim_seen_us = 0;
    claim_send(0);
    assistant_show(app_lang_get()->game_organizing);
    ESP_LOGI(TAG, "%s: looking for the other BOX", s_games[game].id);
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
           tag_in(g->blunder_after, strlen(g->blunder_after), tag->valuestring);
}

/* ---------- what the games used lately ---------- */

/* The items of this game used lately, oldest first, separated by "\n" (allocated; NULL: none) */
static char *used_read(const char *game)
{
    FILE *f = fopen(GAMES_USED_FILE, "r");
    if (!f) {
        return NULL;
    }
    char *out = NULL;
    size_t size = 0;
    FILE *mem = open_memstream(&out, &size);
    char line[160];
    size_t id_len = strlen(game);
    while (mem && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, game, id_len) == 0 && line[id_len] == '\t' && line[id_len + 1]) {
            fprintf(mem, "%s\n", line + id_len + 1);
        }
    }
    fclose(f);
    if (mem) {
        fclose(mem);
    }
    if (out && !out[0]) {
        free(out);
        out = NULL;
    }
    return out;
}

/* One more item used by this game: the file keeps the last GAMES_USED_KEPT lines */
static void used_add(const char *game, const char *item)
{
    char *old = NULL;
    size_t size = 0;
    FILE *f = fopen(GAMES_USED_FILE, "r");
    if (f) {
        FILE *mem = open_memstream(&old, &size);
        char line[160];
        while (mem && fgets(line, sizeof(line), f)) {
            fputs(line, mem);
        }
        if (mem) {
            fclose(mem);
        }
        fclose(f);
    }
    int lines = 0;
    for (const char *p = old; p && *p; p++) {
        lines += *p == '\n';
    }
    const char *from = old;
    for (; from && lines >= GAMES_USED_KEPT; lines--) {
        const char *nl = strchr(from, '\n');
        from = nl ? nl + 1 : from + strlen(from);
    }
    f = fopen(GAMES_USED_FILE, "w");
    if (f) {
        if (from) {
            fputs(from, f);
        }
        fprintf(f, "%s\t%.120s\n", game, item);
        fclose(f);
    }
    free(old);
}

/* The item is in the list ("a\nb\n"), case ignored */
static bool used_has(const char *used, const char *item)
{
    size_t len = strlen(item);
    for (const char *p = used; p && *p; ) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n == len && strncasecmp(p, item, n) == 0) {
            return true;
        }
        p += n + (nl != NULL);
    }
    return false;
}

/* A theme of the game drawn, not one used lately if possible (copied in theme) */
static void theme_draw(const game_def_t *g, const char *used, char *theme, size_t size)
{
    theme[0] = '\0';
    int count = 0;
    for (const char *p = g->themes; p && *p; p++) {
        count += *p == '|';
    }
    count++;
    for (int tries = 0; tries < 12; tries++) {
        int pick = (int)(esp_random() % count);
        const char *p = g->themes;
        for (int i = 0; i < pick; i++) {
            p = strchr(p, '|') + 1;
        }
        p += strspn(p, " ");
        size_t n = strcspn(p, "|");
        while (n && p[n - 1] == ' ') {
            n--;
        }
        snprintf(theme, size, "%.*s", (int)n, p);
        if (!used_has(used, theme)) {
            return;
        }
    }
}

/* The question to the model: the rules, then the lines so far */
static char *line_make(void)
{
    const game_def_t *g = &s_games[s_game];
    const app_lang_t *lang = app_lang_get();
    char *system = NULL;
    char *character = NULL;
    if (!g->plain && s_tone_text) {
        asprintf(&character, "Your character: %s; stay in character. ", s_tone_text);
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
            if (!s_spoke && !g->plain) {
                fputs(" It is your first line: also say in a few words who you are (your character).", out);
            }
        } else {
            fprintf(out, "%s%s", g->first ? g->first : "Start the game.",
                    g->plain ? "" : " In this first line, also say in a few words who you are (your character).");
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
        char *used = used_read(g->id);
        if (g->themes && g->themes[0]) {
            char theme[64];
            theme_draw(g, used, theme, sizeof(theme));
            const char *rule = g->theme_rule && strstr(g->theme_rule, "%s") ? g->theme_rule
                               : "Theme of anything new in this line: %s.";
            const char *at = strstr(rule, "%s");
            fprintf(out, " %.*s%s%s", (int)(at - rule), rule, theme, at + 2);
            ESP_LOGI(TAG, "theme: %s", theme);
            if (!g->secret[0] && cJSON_GetArraySize(s_history) == 0) {
                used_add(g->id, theme);     // a debate's topic: not drawn again soon
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
    if (event == TTS_EVENT_FIRST_AUDIO) {
        ui_ctrl_reply_set_audio_start_flag(true);   // the line scrolls with its speech
    } else if (event == TTS_EVENT_DONE) {
        s_spoken = true;
    }
}

/*
 * The tag a line may start with ("RIGHT: Correct !"), asked by the rules: an upper-case word and a colon. Returns
 * the text after it (and its spaces), the text itself without a tag; its length in *tag_len (0: none)
 */
static const char *tag_skip(const char *text, size_t *tag_len)
{
    const char *p = text;
    while (*p == ' ' || *p == '\n') {
        p++;
    }
    const char *word = p;
    while (*p >= 'A' && *p <= 'Z') {
        p++;
    }
    size_t len = p - word;
    while (*p == ' ') {
        p++;
    }
    if (len < 2 || len > 12 || *p != ':') {
        *tag_len = 0;
        return text;
    }
    *tag_len = len;
    p++;
    while (*p == ' ' || *p == '\n') {
        p++;
    }
    return p;
}

/* short_after: a line starting with this tag keeps only its first sentence (a riddle guessed: "Correct !" and
   nothing more, the one who found it asks the next one) */
static void line_shorten(char *text, const char *short_after)
{
    size_t tag_len = 0;
    char *body = (char *)tag_skip(text, &tag_len);
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
    const game_def_t *g = &s_games[s_game];
    if (!g->short_after[0] && !g->turns[0]) {
        return;     // a game without tags
    }
    char *keep = NULL;
    for (char *p = text; *p; p++) {
        if (p == text || p[-1] == '\n') {
            size_t tag_len = 0;
            tag_skip(p, &tag_len);
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
    const char *secret = s_games[s_game].secret;
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
                    used_add(s_games[s_game].id, answer);
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
    tag_of(text, tag, sizeof(tag));
    cJSON *m = msg_new("speak");
    cJSON_AddNumberToObject(m, "turn", turn);
    cJSON_AddStringToObject(m, "tag", tag);
    send_all(m);
    size_t tag_len;
    const char *said = tag_skip(open, &tag_len);
    ui_ctrl_reply_show_scrolled(said);
    s_spoken = false;
    tts_stream_begin(on_speech);
    tts_stream_say(said);
    tts_stream_end();
    int64_t t0 = esp_timer_get_time();
    while (!s_spoken && !s_stop_requested && esp_timer_get_time() - t0 < 60 * 1000000LL) {
        vTaskDelay(pdMS_TO_TICKS(50));
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
    s_spoke = true;
    electing_start(s_my_id);
}

/* The line of another BOX listened to (and its "done" received): kept, then this BOX claims the next one */
static void line_heard(void)
{
    const char *text = s_heard && s_heard[0] ? s_heard : s_done_text ? s_done_text : "";
    ESP_LOGI(TAG, "line %d (player %d, %s): %s%s", s_turns + 1, player_number(s_speaker), s_line_tag,
             text, s_heard && s_heard[0] ? "" : " (not heard: the words sent)");
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

/* Elected: speak (the next line, or the end after the last one), else wait for the one elected */
static void decided(uint64_t winner)
{
    ESP_LOGI(TAG, "line %d: player %d%s", s_turns + 1, player_number(winner), winner == s_my_id ? " (this BOX)" : "");
    s_reclaim_us = 0;
    s_since = esp_timer_get_time();
    if (s_turns >= s_turns_max) {
        if (winner == s_my_id) {
            send_simple("stop", s_turns, NULL);
            game_end(app_lang_get()->game_end_natural);
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
        snprintf(title, sizeof(title), app_lang_get()->game_title, s_games[s_game].name);
        assistant_show(title);
        ESP_LOGI(TAG, "%s: %d BOX, %s, %d lines at most", s_games[s_game].id, s_player_count,
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
                ESP_LOGI(TAG, "%s: joined", s_games[game].id);
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
        } else if (strcmp(m->type, "speak") == 0 && m->turn == s_turns + 1 && s_state != G_SPEAKING) {
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

void game_poll(void)
{
    game_msg_t m;
    while (s_queue && xQueueReceive(s_queue, &m, 0) == pdTRUE) {
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
            tag_of(text, tag, sizeof(tag));
            if (!tag[0] || !tag_in(allowed, strlen(allowed), tag)) {
                ESP_LOGW(TAG, "line tagged \"%s\" instead of %s: asked again", tag, allowed);
                free(text);
                text = line_make();
            }
        }
        if (!text) {
            send_simple("stop", s_turns, NULL);
            game_end(app_lang_get()->game_peer_left);
            return;
        }
        line_clean(text);
        line_shorten(text, s_games[s_game].short_after);
        line_say(text);
        free(text);
        break;
    }
    default:
        break;
    }
}
