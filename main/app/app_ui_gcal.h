/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Screen linking Google Calendar, opened from the Paramètres page of the Settings screen: a QR code to
 * google.com/device, the code to type there, and where it stands (waiting, linked, refused, expired). Back to the
 * Settings screen once linked, or with its arrow (the linking cancelled).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Open the screen and start linking (display locked); back: done() called (Settings screen to refresh) */
void ui_gcal_open(void (*done)(void));

#ifdef __cplusplus
}
#endif
