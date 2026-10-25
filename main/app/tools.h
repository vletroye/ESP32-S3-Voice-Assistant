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

#ifdef __cplusplus
}
#endif
