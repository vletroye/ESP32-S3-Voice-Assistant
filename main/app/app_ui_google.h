/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Screen linking the Google account, opened from the Paramètres page of the Settings screen: a QR code to Google's
 * consent page (google_auth.c), and the steps: accept on the phone, copy the address of the page that follows (it
 * does not open), paste it in CONFIG.INI as Google_code, restart within 10 minutes. Back to the Settings screen with
 * its arrow.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Open the screen (display locked); back: done() called (Settings screen to refresh) */
void ui_google_open(void (*done)(void));

#ifdef __cplusplus
}
#endif
