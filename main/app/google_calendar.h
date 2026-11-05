/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Google Calendar, read only: linked once with Google's flow for devices without a keyboard (OAuth 2.0 "TVs and
 * limited input devices"): the device shows a QR code to google.com/device and a short code, the user accepts on
 * the phone, the device gets a refresh token (kept in /state/gcal.json). Then the model's tool calendar_events
 * reads the events of a day. The OAuth client is the one of the project's Google Cloud project
 * (google_oauth_client.json at build time, never committed); without it the calendar is not offered.
 */

#pragma once

#include <stdbool.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where the linking stands (shown by its screen) */
typedef enum {
    GCAL_LINK_IDLE,         /* not linking */
    GCAL_LINK_ASKING,       /* asking Google for a code */
    GCAL_LINK_WAITING,      /* the code shown: waiting for the user to accept on the phone */
    GCAL_LINK_DONE,         /* linked */
    GCAL_LINK_DENIED,       /* the user refused */
    GCAL_LINK_EXPIRED,      /* the code expired before it was accepted */
    GCAL_LINK_FAILED,       /* Google could not be reached, or refused the client */
} gcal_link_state_t;

/** The firmware was built with the OAuth client (google_oauth_client.json) */
bool gcal_configured(void);

/** A Google account is linked (a refresh token kept) */
bool gcal_linked(void);

/** Read the refresh token kept (after state_mount) */
void gcal_init(void);

/** Start linking (the main task asks for a code at its next turn); cancel: gcal_link_cancel */
void gcal_link_start(void);
void gcal_link_cancel(void);

/** The state, the address to open (QR code) and the code to type there */
gcal_link_state_t gcal_link_state(const char **url, const char **code);

/** Called every second by the main task: asks for the code, then whether the user accepted (every few seconds) */
void gcal_poll(void);

/** Forget the account (the token removed, and revoked at Google by the main task): not linked from now on */
void gcal_unlink(void);

/**
 * @brief The model's tool calendar_events: the events of `days` days from `date` ("2026-11-05", NULL: today), as
 *        a small JSON for the model; an error JSON (is_error) when not linked or not reachable
 */
char *gcal_events(const char *date, int days, bool *is_error);

/** The tool's definition (for tools.c), NULL when the calendar is not offered */
const char *gcal_tool_definition(void);

#ifdef __cplusplus
}
#endif
