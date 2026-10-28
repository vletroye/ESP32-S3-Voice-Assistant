/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Radio-Browser (https://www.radio-browser.info): the free, community-maintained directory of internet radios.
 * Stations searched by name, genre (tag) or country, most listened first, keeping only the streams the radio can
 * play (MP3 or AAC over http, not HLS, at most 192 kbit/s, online at the last check).
 */

#pragma once

#include <stdbool.h>
#include "radio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Stations kept from a search */
#define RADIO_BROWSER_MAX   20

/**
 * @brief Search (any of name, tag, countrycode may be NULL or ""; countrycode: 2 letters, "BE"), most listened
 *        first, into out (up to max); returns the stations found, -1 when no server answered. Blocking (~1 s).
 */
int radio_browser_search(const char *name, const char *tag, const char *countrycode, radio_station_t *out, int max);

/** Count a listen of the station (the directory's popularity), as the API asks for each station played */
void radio_browser_click(const char *uuid);

#ifdef __cplusplus
}
#endif
