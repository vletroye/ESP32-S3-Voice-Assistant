/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * A timer running (alarms.c): its countdown (m:ss, the nearest timer) above the robot's head, asleep, listening and
 * thinking; asleep, its eyes become two split flaps, like the departure board of a station, turning over with the
 * seconds of the countdown (the closed eyes and the Z come back once no timer runs).
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Room taken above the screen of the eyes by the countdown, over the provider's icon (for its redraw area) */
#define UI_TIMER_LABEL_H    22

/**
 * @brief Make the countdowns and the flaps, refreshed by an LVGL timer (display locked, once the screens and the
 *        icons on the foreheads exist)
 *
 * @param above_y  Distance from the top of a screen of the eyes to the bottom of the countdown
 */
void ui_timer_init(lv_coord_t above_y);

#ifdef __cplusplus
}
#endif
