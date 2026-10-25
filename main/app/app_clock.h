/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Clock shown under the sleeping robot: HH:MM in seven-segment digits and the date (DDD DD/MM/YY), with the
 * speaker volume at the bottom left (bar, icon, percentage) and the Wi-Fi signal at the bottom right.
 */

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set the time zone and start the time synchronization (SNTP, done once Wi-Fi is up).
 *
 * @param tz POSIX TZ string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"
 */
void app_clock_sync_start(const char *tz);

/**
 * @brief Create the clock on the sleep panel, refreshed every second. Call with the display locked.
 *
 * @param parent    Panel shown while the robot sleeps
 * @param weekdays  Day abbreviations, Sunday first
 */
void app_clock_create(lv_obj_t *parent, const char *const weekdays[7]);

#ifdef __cplusplus
}
#endif
