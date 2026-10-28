/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Internet radio: an MP3 / AAC stream read over HTTP, decoded and played through the speaker (16 kHz, like the
 * speech). The radio is suspended while the assistant listens or speaks, and resumed when it goes back to sleep;
 * paused by the user, it stays silent until resumed.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RADIO_OFF,          /* no station, or it could not be played */
    RADIO_PLAYING,      /* playing (or connecting) */
    RADIO_SUSPENDED,    /* stopped by a conversation: resumed when the assistant goes back to sleep */
    RADIO_PAUSED,       /* stopped by the user: resumed on request */
} radio_state_t;

/** Play this station (its name as said and shown, its stream URL); the one playing is replaced. Does not wait. */
void radio_play(const char *name, const char *url);

/** The assistant needs the speaker (wake word, touch, speech): the radio stops until radio_on_sleep(). Waits until
 *  the speaker is free (~100 ms). */
void radio_suspend(void);

/** The assistant went back to sleep: a suspended radio plays again. Does not wait. */
void radio_on_sleep(void);

/** Stopped by the user ("stop"): silent until radio_resume(). Waits until the speaker is free. */
void radio_pause(void);

/** The current station again (paused, suspended or failed); false without a station. Does not wait. */
bool radio_resume(void);

radio_state_t radio_state(void);

/** A stream plays (or connects) */
bool radio_playing(void);

/** The name of the current station ("" without one) */
const char *radio_station_name(void);

/** The title being played (ICY metadata of the stream: "Artist - Title"), "" when the station gives none */
void radio_title(char *buf, size_t size);

/** Treble of the radio in dB (-12..6; negative: softer, 0: unchanged), applied at once (Radio_treble at start-up) */
void radio_set_treble(int db);

#ifdef __cplusplus
}
#endif
