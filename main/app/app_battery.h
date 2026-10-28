/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Battery of the ESP32-S3-BOX-3-SENSOR dock (18650 cell): its voltage on GPIO10 (ADC1 channel 9, through a 1:4
 * divider), read only when the dock is there (its presence radar answered).
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Sections of the battery shown (1..5); 0: no battery (no dock, or a voltage out of the range of a cell) */
#define APP_BATTERY_SECTIONS    5

/**
 * @brief Read the voltage again (16 samples, smoothed) and give the sections lit: 1 below 3.50 V (critical), 2 from
 *        3.50 V, 3 from 3.70 V, 4 from 3.80 V, 5 from 3.95 V (a little hysteresis); 0 without battery.
 *        Fast (a few hundred microseconds): called from the LVGL task.
 */
int app_battery_sections(bool *critical);

#ifdef __cplusplus
}
#endif
