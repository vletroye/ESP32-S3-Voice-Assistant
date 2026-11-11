/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Games between BOX, shared by their three files (see game.h):
 * - game_defs.c: the games (the firmware's, merged with GAMES.INI), the phrases starting them, the tags of the lines
 *   and the rules on them, what the games used lately;
 * - game_net.c: the UDP messages between the BOX of the network;
 * - game.c: the game itself (the BOX of a game, who speaks, the lines made, said, listened to).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#define GAME_TEXT_MAX       1200        /* a line, at most (UDP) */

/* ---------- game_defs.c ---------- */

/* A game: the firmware's (main/games/games.ini), merged key by key with GAMES.INI */
typedef struct {
    char id[24];
    char name[64];
    char phrases[400];      /* of the current language, "a | b | c" */
    bool name_own, phrases_own;     /* of the current language (else the English ones, until it comes) */
    char *first;
    char *rules;
    bool plain;             /* character = no: no character */
    char short_after[16];   /* a tag: a line starting with "TAG:" keeps only its first sentence */
    char secret[16];        /* a tag: what follows "TAG:" is kept for itself (the answer of its riddle) */
    char secret_with[32];   /* the tags of the lines that must give it ("RIDDLE"): asked again once if missing */
    char blunder_after[48]; /* blunder_rule only after a line with one of these tags */
    int blunder;            /* percent of the lines given blunder_rule */
    char *blunder_rule;     /* a wrong guess on purpose, with humour (the riddles) */
    char turns[64];         /* "GUESS>RIDDLE WRONG; RIGHT>GUESS": who says the line after a tag (else the first claim) */
    char next[96];          /* "RIDDLE>GUESS; GUESS>RIGHT WRONG": the tags a line may have after a tag */
    char *themes;           /* "a | b | c": one drawn for each line, given by theme_rule */
    char *theme_rule;       /* "... its theme is: %s ..." */
    char theme_for[32];     /* "first": only the first line gets it (the debate keeps its topic); tags ("THINK"):
                               only the lines that may have one of them (and the first); else every line */
    char *characters;       /* "a | b | c": this game's characters (Game_tone = random), else the firmware's list */
    char *last;             /* the instruction of the last line */
    int sentences;          /* each line cut after this many sentences (0: not cut) */
    int lines;              /* this game's number of lines (a guessing game is longer), else Game_turns_max */
    bool reasoning;         /* reasoning = yes: its lines and document made in the reasoning mode */
    char document[24];      /* the file written at the end by the BOX ending the game ("project.txt") */
    char *document_rule;    /* what it is */
} game_def_t;

/** Once at start-up: GAMES.INI installed (as a help) when missing or emptied */
void games_init(void);

/** The games read again when the language changed (their names and phrases) */
void games_load(void);

/** A game by its id ("debate"), -1 if none */
int game_find(const char *id);

/** A game (0 to the number of games - 1) */
const game_def_t *game_def(int game);

/** The game whose phrase begins what was said, -1 if none (any task) */
int game_phrase(const char *text);

/** The items of a list "a | b | c" */
int game_list_count(const char *list);

/** One of them (index from 0), trimmed, into out; false if there is none */
bool game_list_item(const char *list, int index, char *out, size_t size);

/**
 * The tag a line may start with ("RIGHT: Correct !"): an upper-case word and a colon. Returns the text after it (and
 * its spaces), the text itself without a tag; its length in *tag_len (0: none)
 */
const char *game_tag_skip(const char *text, size_t *tag_len);

/** The tag of a line ("GUESS"), "" if none */
void game_tag_of(const char *text, char *tag, size_t size);

/** A word of a list of tags ("RIDDLE WRONG") */
bool game_tag_in(const char *list, size_t list_len, const char *tag);

/** The right side of the rule of this tag in "A>B C; D>E" (copied in out; false: no rule) */
bool game_rule_of(const char *rules, const char *tag, char *out, size_t size);

/** The items of this game used lately, oldest first, one per line (allocated; NULL: none) */
char *game_used_read(const char *game);

/** One more item used by this game (the last ones kept) */
void game_used_add(const char *game, const char *item);

/** A theme of the game drawn, not one used lately if possible (copied in theme) */
void game_theme_draw(const game_def_t *g, const char *used, char *theme, size_t size);

/* ---------- game_net.c ---------- */

/*
 * A message received, given by the network task to the main task; "local": the listening of this BOX ended (its
 * transcript in text, NULL if nothing heard), from the SR handler task (game_heard); "start": the phrase of a game
 * heard here (its index in turn)
 */
typedef struct {
    char type[8];           /* claim, join, speak, done, heard, stop, local, start */
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

/** The socket and the task reading it; false if the UDP port could not be opened */
bool game_net_init(void);

/** This BOX's id (its MAC) */
uint64_t game_net_id(void);

/** A message to send: its type, this BOX's id, the game's nonce (to be sent with game_net_send) */
cJSON *game_net_msg(const char *type, uint32_t nonce);

/** To every BOX of the network (sent a few times), then deleted */
void game_net_send(cJSON *msg);

/** A message of this BOX for the main task (any task); false if the queue is full */
bool game_net_post(const game_msg_t *msg);

/** The next message for the main task, without waiting; false if none */
bool game_net_receive(game_msg_t *msg);

/** The time of a claim: ms since 1970, the same on every BOX (SNTP) */
int64_t game_now_ms(void);
