/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Speaker volume changed by voice. Writing the codec registers while a sound plays makes crackles, so a new
 * level is applied once the reply has been spoken, and saved to NVS by a task that may write to flash.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Volume read from NVS at start-up (0..100), applied at once */
void app_volume_init(int level);

/** Current level, or the level waiting to be applied */
int app_volume_get(void);

/** Ask for a new level (clamped to 0..100), applied by app_volume_apply_pending() */
int app_volume_request(int level);

/** Apply the requested level, if any (call when no sound plays) */
void app_volume_apply_pending(void);

/** Save the level to NVS if it changed (call from a task with an internal RAM stack) */
void app_volume_save(void);

#ifdef __cplusplus
}
#endif
