/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Games between two BOX or more on the same network ("disputez-vous", "concours de devinettes"): they take turns,
 * each with its own model and voice, until one is touched (or the wake word said to one speaking), or
 * Game_turns_max lines.
 *
 * The games are read from /spiffs/games.ini (GAMES.INI of the configuration drive; the firmware's own installed when
 * missing): the phrases that start them, the instruction of the first line and the rules of every line. The BOX
 * agree by UDP broadcast on the local network (port GAME_PORT), but hear the lines with their microphones:
 * - the BOX that heard the phrase claim the first line with their time (SNTP), the asleep ones join; the earliest
 *   claim speaks (a tie: the BOX tied claim again after a random delay);
 * - the one speaking sends "speak" (its line's tag), the others listen to it (ended by 1.5 s of silence at least),
 *   then each claims the next line once it has heard it: the earliest claim says it (or the BOX a rule of the game
 *   names: the riddles' asker judges a guess); "done" carries the words, used only by a BOX that heard nothing;
 * - after the last line, the BOX elected ends the game ("Ok, c'est bon. Arrêtons-nous là.").
 * The personality of each one is the one of PERSO_<XX>.TXT (Game_tone = persona) or one drawn from the firmware's
 * list (Game_tone = random, the default), not one drawn by another BOX.
 *
 * Three files (game_internal.h): game_defs.c (the games, GAMES.INI), game_net.c (the UDP messages), game.c (the
 * game). The turns run on the main task (game_poll), as the model test: each line is a question to the model in
 * use, apart from the conversation (llm_ask_apart).
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GAME_PORT   47110

/** Read the games, open the network socket (once the Wi-Fi may come: it waits for it) */
void game_init(void);

/**
 * @brief What the user said starts a game ("disputez-vous"): looking for the other BOX, the game then played by
 *        game_poll(). False when it is not a game's phrase.
 */
bool game_try_start(const char *text);

/** A game is going on (or being set up) */
bool game_active(void);

/** Stop it, on both BOX (the wake word, a touch): true if one was going on */
bool game_stop(void);

/** Called by the main task (every 100 ms during a game, and at once when a message comes): its next step */
void game_poll(void);

/** A listening of the game (another BOX's line) is asked or under way: its transcript goes to game_heard() */
bool game_listening(void);

/** The transcript of that listening (allocated, taken; NULL: nothing heard), from the SR handler task */
void game_heard(char *text);

#ifdef __cplusplus
}
#endif
