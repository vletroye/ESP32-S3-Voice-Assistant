/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Google Calendar for the model (the account of google_auth.c, the main calendar): the events of a period (to answer
 * "what do I have tomorrow", "am I free on Thursday afternoon"), creating one, moving or renaming one, deleting one.
 * Moving and deleting are done only with confirmed=true: else the event concerned is given back, for the model to ask
 * the user first. Times are local (the device's time zone).
 */

#pragma once

#include <stdbool.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The tools' definitions (a JSON array, Claude's format), NULL when Google is not configured in CONFIG.INI */
const char *google_calendar_definitions(void);

/** The tool is one of these */
bool google_calendar_has(const char *name);

/** Run one: the result for the model (allocated, to be freed) */
char *google_calendar_run(const char *name, const cJSON *input, bool *is_error);

#ifdef __cplusplus
}
#endif
