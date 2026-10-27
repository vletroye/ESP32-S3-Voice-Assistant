/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Home automation through the MCP server of a Homey Pro on the local network (e.g. the "MCP AI Bridge" app),
 * MCP over streamable HTTP with JSON answers. A selection of its tools is offered to Claude: devices, zones,
 * moods, flows, presence, energy (nothing that deletes, installs or reconfigures).
 */

#pragma once

#include <stdbool.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Connect to the server and read its tools (call once the Wi-Fi is up, from a task with an internal
 *        RAM stack). Does nothing if no URL is configured.
 *
 * @return Definitions of the selected tools in Claude's format (JSON array, owned by the caller), NULL if none
 */
cJSON *homey_mcp_load_tools(const char *url, const char *key);

/**
 * @brief Homey device classes (comma-separated, "socket" for power plugs) that are not turned off without a
 *        confirmation: such a request first answers "confirmation_required" to the model, which asks the user and
 *        calls again with confirmed=true. NULL or "" for none.
 */
void homey_mcp_set_confirm_off(const char *classes);

/** The tool is one of Homey's */
bool homey_mcp_has_tool(const char *name);

/**
 * @brief Call a Homey tool.
 *
 * @return Text result for the model (allocated, to be freed)
 */
char *homey_mcp_call(const char *name, const cJSON *input, bool *is_error);

#ifdef __cplusplus
}
#endif
