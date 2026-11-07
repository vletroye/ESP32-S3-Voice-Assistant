/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Gmail for the model (the account of google_auth.c): the recent or unread mails, a search (Gmail's search words),
 * reading one (its text, HTML made plain, cut when long), sending one or a reply (only with confirmed=true: else the
 * mail is given back, for the model to read it to the user first; the address must be a whole one), and marking
 * mails read or unread, archiving them or putting them in the bin.
 */

#pragma once

#include <stdbool.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The tools' definitions (a JSON array, Claude's format), NULL when Google is not configured in CONFIG.INI */
const char *gmail_definitions(void);

/** The tool is one of these */
bool gmail_has(const char *name);

/** Run one: the result for the model (allocated, to be freed) */
char *gmail_run(const char *name, const cJSON *input, bool *is_error);

#ifdef __cplusplus
}
#endif
