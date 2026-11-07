/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * The "Système" screen, opened by a long press on the Settings icon of the home screen: what the device is and how it
 * stands (firmware version and date, board and dock, processor, flash, PSRAM and internal RAM with what is free, the
 * files' partitions, the NVS, ESP-IDF, the MAC address, the time since the start and why it started), refreshed every
 * 2 seconds.
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called once the screens exist (display locked): the long press on the Settings icon opens it */
void ui_system_init(void);

/** This screen is the one shown (read without the display lock) */
bool ui_system_shown(void);

/** The assistant listens or answers: back from this screen (display locked) */
void ui_system_close(void);

#ifdef __cplusplus
}
#endif
