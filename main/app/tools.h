/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Tools the model can call: their definitions (Claude format) and their execution on the device.
 */

#pragma once

#include <stdbool.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Tool definitions: JSON array of {name, description, input_schema}, owned by this module */
const cJSON *tools_definitions(void);

/**
 * @brief Run a tool call.
 *
 * @return Result for the model (allocated, to be freed), NULL if the tool is unknown
 */
char *tools_run(const char *name, const cJSON *input, bool *is_error);

/** Forget the requests of the previous answer (call before each question) */
void tools_reset_requests(void);

/** A tool asked to go back to sleep after the answer instead of listening for a follow-up (volume change) */
bool tools_sleep_after_reply(void);

#ifdef __cplusplus
}
#endif
