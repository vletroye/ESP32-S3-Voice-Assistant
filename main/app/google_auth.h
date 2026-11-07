/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * The user's Google account, for Google Tasks (and later Calendar): OAuth 2.0 for an installed app, with the
 * client of the user's own Google Cloud project (Google_client and Google_secret of CONFIG.INI, type "Desktop app").
 * Linked once, from a computer (Google blocks this flow in a phone's browser): on the way to the configuration mode,
 * GOOGLE.HTM is put on its drive, opening Google's consent page; once accepted, Google sends the browser to
 * http://localhost/?code=... (a page that does not open); that address, pasted as Google_code in CONFIG.INI, is
 * exchanged at the next start for a refresh token, kept in /state/google.json (never shown nor logged), and
 * Google_code is erased. Then the device reaches Google alone: an access token of an hour is asked with
 * the refresh token whenever needed.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where it stands, for the Settings screen */
typedef enum {
    GOOGLE_NOT_CONFIGURED,  /* no Google_client / Google_secret in CONFIG.INI */
    GOOGLE_NOT_LINKED,      /* configured, no account linked yet */
    GOOGLE_LINKING,         /* a Google_code to exchange (once the Wi-Fi is up) */
    GOOGLE_LINKED,
    GOOGLE_FAILED,          /* the code was refused (expired, used, wrong client): to link again */
} google_state_t;

/** Read the client, the refresh token kept and a Google_code to exchange (after settings and state_mount) */
void google_auth_init(void);

google_state_t google_auth_state(void);

/** The address of Google's consent page, "" when not configured */
void google_auth_consent_url(char *out, size_t size);

/**
 * @brief On the way to the configuration mode: GOOGLE.HTM (/spiffs/google.htm) opening the consent page, when Google
 *        is configured and not linked; removed otherwise
 */
void google_auth_export_link(void);

/** Called every second by the main task: exchanges a Google_code once the Wi-Fi is up, revokes after an unlink */
void google_auth_poll(bool wifi_up);

/** Forget the account (revoked at Google by the main task): not linked from now on */
void google_auth_unlink(void);

/**
 * @brief A request to a Google API with the account's access token (refreshed when needed): GET, POST, PATCH or
 *        DELETE, a JSON body or NULL.
 *
 * @param status  HTTP status, -1 when Google was not reached, 401 when the account is not linked or was withdrawn
 * @return The response (allocated, to be freed), NULL if none
 */
char *google_api(const char *method, const char *url, const char *json_body, int *status);

#ifdef __cplusplus
}
#endif
