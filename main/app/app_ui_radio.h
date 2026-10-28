/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Radio screen, opened by a tap on the volume of the sleep screen: the favourites (scrolled, a tap plays one), the
 * station playing with its heart (a tap adds it to the favourites or removes it), the volume and the treble, and
 * the buttons volume -/+, previous, play / pause, next, treble -/+.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Called once the screens exist (display locked) */
void ui_radio_init(void);

/** The assistant listens or answers: back from the radio screen (display locked) */
void ui_radio_close(void);

#ifdef __cplusplus
}
#endif
