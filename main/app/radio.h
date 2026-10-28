/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Internet radio: an MP3 / AAC stream read over HTTP, decoded and played through the speaker (16 kHz stereo,
 * like the speech). Phase 0: a stream played from its URL, stopped by the wake word, with measures in the log.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Play the stream of this URL (the one playing is stopped first) */
void radio_play(const char *url);

/** Stop the stream (waits until the speaker is free); nothing if none plays */
void radio_stop(void);

/** A stream plays */
bool radio_playing(void);

/** Treble of the radio in dB (-12..6; negative: softer, 0: unchanged), applied at once (Radio_treble at start-up) */
void radio_set_treble(int db);

#ifdef __cplusplus
}
#endif
