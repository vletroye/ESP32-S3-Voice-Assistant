/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Screen of the Wi-Fi and the battery, opened by a tap on them at the bottom right of the sleep screen: the network
 * (name, signal, channel, access point, security, addresses) and the battery of the BOX-3-SENSOR dock (level,
 * voltage, charging or not), refreshed every 2 s.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Called once the screens exist (display locked) */
void ui_status_init(void);

/** The assistant listens or answers: back from this screen (display locked) */
void ui_status_close(void);

#ifdef __cplusplus
}
#endif
