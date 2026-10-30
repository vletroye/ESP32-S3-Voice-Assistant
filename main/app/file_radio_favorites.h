/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Favourite radios: /spiffs/radio.txt (RADIO.TXT of the configuration drive), one station per line,
 * "name | stream URL | Radio-Browser id" (the id is optional); added and removed by voice or from the radio
 * screen, editable from the PC (comments kept).
 */

#pragma once

#include <stdbool.h>
#include "radio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RADIO_FAVORITES_FILE    "/spiffs/radio.txt"

/** Read the file (made with its header when missing) */
void radio_favorites_init(void);

/** The favourites, in the order of the file (copied into out, up to max); returns their number */
int radio_favorites_list(radio_station_t *out, int max);

int radio_favorites_count(void);

/** The station is a favourite (same stream URL) */
bool radio_favorites_contains(const radio_station_t *station);

/** Added at the end of the file; false when it already is one, or the list is full */
bool radio_favorites_add(const radio_station_t *station);

/** Removed from the file; false when it was not one */
bool radio_favorites_remove(const radio_station_t *station);

/** The favourite named in a normalized text ("nostalgie" for "Nostalgie Belgique"): its index, -1 if none */
int radio_favorites_find(const char *norm);

#ifdef __cplusplus
}
#endif
