/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Find the presence radar of the BOX-3-SENSOR dock and switch the screen off after screen_off_s seconds
 *        without presence while the assistant sleeps (0: the screen stays on). After the display is started.
 */
void app_presence_init(int screen_off_s);

/** Someone is there (wake word): the screen goes on, the delay starts again */
void app_presence_activity(void);

/** The ESP32-S3-BOX-3-SENSOR dock is there (its radar answered at start-up, whatever Screen_off_s) */
bool app_presence_dock(void);

#ifdef __cplusplus
}
#endif
