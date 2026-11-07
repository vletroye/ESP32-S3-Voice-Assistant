/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * The "Système" page of the Settings screen: what the device is and how it stands (firmware version and date, board
 * and dock, processor, flash, PSRAM and internal RAM with what is free, the files' partitions, the NVS, ESP-IDF, the
 * MAC address, the time since the start and why it started), refreshed each time the page is shown.
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Its table, in the panel of the Settings screen (display locked) */
lv_obj_t *ui_system_create(lv_obj_t *parent);

/** Filled again (display locked) */
void ui_system_fill(lv_obj_t *table);

#ifdef __cplusplus
}
#endif
