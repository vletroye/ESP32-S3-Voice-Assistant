/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Google Tasks for the model (the account of google_auth.c): its lists, the open tasks of a list, adding several
 * at once (the shopping list), completing, reopening, deleting or renaming one. Lists and tasks are named as the user
 * says them (case and accents ignored); the API's ids are not shown to the model.
 */

#pragma once

#include <stdbool.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The tools' definitions (a JSON array, Claude's format), NULL when Google is not configured in CONFIG.INI */
const char *google_tasks_definitions(void);

/** The tool is one of these */
bool google_tasks_has(const char *name);

/** Run one: the result for the model (allocated, to be freed) */
char *google_tasks_run(const char *name, const cJSON *input, bool *is_error);

#ifdef __cplusplus
}
#endif
