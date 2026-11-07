/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Screen of the timers, alarms and reminders (alarms.c), opened by a tap on the clock of the sleep screen: the timers
 * (their end and the time left), the alarms and the reminders (their next day and time, label, repetition, radio),
 * refreshed every second.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Called once the screens exist (display locked) */
void ui_alarms_init(void);

/** The assistant listens or answers: back from this screen (display locked) */
void ui_alarms_close(void);

#ifdef __cplusplus
}
#endif
