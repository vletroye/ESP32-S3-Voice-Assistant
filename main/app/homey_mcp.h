/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * What is particular to the MCP server of a Homey Pro (e.g. the "MCP AI Bridge" app), reached by mcp_client.c: the
 * tools offered when none are chosen (everyday home control: devices, zones, moods, flows, presence, energy, nothing
 * that deletes, installs or reconfigures), the confirmation before turning off some devices (Confirm_off), and the
 * fields of no use by voice removed from its results.
 */

#pragma once

#include <stdbool.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A tool call on the same server (for the devices that are on, before asking a confirmation) */
typedef char *(*homey_mcp_call_t)(void *server, const char *tool, const cJSON *input, bool *is_error);

/** The tool is one of Homey's everyday ones (offered when its tools are not chosen in CONFIG.INI) */
bool homey_mcp_default_tool(const char *tool);

/**
 * @brief Homey device classes (comma-separated, "socket" for power plugs) that are not turned off without a
 *        confirmation: such a request first answers "confirmation_required" to the model, which asks the user and
 *        calls again with confirmed=true. NULL or "" for none.
 */
void homey_mcp_set_confirm_off(const char *classes);

/** A tool's definition (Claude's format) adapted: the "confirmed" argument added to those turning devices off */
void homey_mcp_adapt_definition(const char *tool, cJSON *definition);

/**
 * @brief Before a call: a zone_id given as a name ("bureau") is replaced by the zone's id (zones_list); a request
 *        turning off devices to confirm, not confirmed yet, or a zone not found, gets the answer for the model
 *        instead (allocated); NULL to go on. The arguments are then cleaned (without "confirmed", unknown to Homey).
 */
char *homey_mcp_before_call(const char *tool, cJSON *arguments, homey_mcp_call_t call, void *server);

/** A result without the fields of no use by voice (text taken over, the new one returned) */
char *homey_mcp_compact(char *text);

#ifdef __cplusplus
}
#endif
