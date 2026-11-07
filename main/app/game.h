/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Games between two BOX on the same network ("disputez-vous", "concours de devinettes"): the two take turns, each
 * with its own model and voice, until the wake word is said to either of them (or Game_turns_max lines).
 *
 * The games are read from /spiffs/games.ini (GAMES.INI of the configuration drive; the firmware's own installed when
 * missing): the phrases that start them, the instruction of the first line and the rules of every line. The two BOX
 * find each other by UDP broadcast on the local network (port GAME_PORT) and send their lines as text: a line is sent
 * before it is spoken, so that the other prepares its answer meanwhile, and "done" once spoken, so that the other
 * speaks only then. The personality of each one is the one of PERSO_<XX>.TXT (Game_tone = persona) or one drawn from
 * the firmware's list (Game_tone = random, the default), never the same for both.
 *
 * The turns run on the main task (game_poll), as the model test: each line is a question to the model in use, apart
 * from the conversation (llm_ask_apart).
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

/** Called by the main task (every second, and at once when a message comes): the next step of the game */
void game_poll(void);

#ifdef __cplusplus
}
#endif
