/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The free models of the last list read (SPIFFS), offered at start-up; before the Settings screen is built */
void openrouter_free_init(void);

/**
 * @brief Read the live list of OpenRouter if it is more than a day old (blocking, a few seconds; Wi-Fi up,
 *        clock set). From a task whose stack is in internal RAM (the list is saved to SPIFFS).
 *
 * @return true when the free models changed (the Settings screen is to be refreshed)
 */
bool openrouter_free_refresh(void);

#ifdef __cplusplus
}
#endif
