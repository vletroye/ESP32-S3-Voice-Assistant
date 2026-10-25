/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Weather from Open-Meteo (free, no API key): the place is found with its geocoding service, then the
 * current conditions and the daily forecast are fetched.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Weather of a place, as a compact JSON text for the model.
 *
 * @param location  Place name, optionally followed by ", country" (e.g. "Namur, Belgique")
 * @param days      Days of forecast, 1 (today) to 7
 * @param lang      ISO-639-1 language for the place names
 * @param is_error  Set to true when the result is an error description
 * @return Allocated JSON text (to be freed)
 */
char *weather_get(const char *location, int days, const char *lang, bool *is_error);

/** Read the places already looked up (/spiffs/places.json), once at start-up */
void weather_places_load(void);

/** Save the places if one was added (call when idle, from a task with an internal RAM stack) */
void weather_places_save(void);

#ifdef __cplusplus
}
#endif
