/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * A timer running (alarms.c): its countdown (m:ss, the nearest timer) at the top middle of the screen, as the costs
 * are at the top right, with the robot (asleep, listening, thinking), not over a reply; asleep, the robot's eyes become two stopwatches whose hands turn (an animation, not the
 * countdown). The closed eyes and the Z come back once no timer runs.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Make the countdown and the stopwatches, refreshed by an LVGL timer (display locked, once the screens exist) */
void ui_timer_init(void);

#ifdef __cplusplus
}
#endif
