/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * MCP servers (streamable HTTP, JSON or event-stream answers) whose tools are offered to the model: those of the
 * [MCP] section of CONFIG.INI (Mcp1_url, Mcp1_key, Mcp1_name, Mcp1_tools, Mcp1_on, then Mcp2_..., up to Mcp99_),
 * and the Homey Pro of Homey_url / Homey_key. Each one can be switched off on the MCP page of the Settings screen
 * (McpN_on, Homey_on): its tools are then no longer offered and it is not contacted.
 *
 * McpN_tools chooses the tools offered (they cost tokens at each question): names separated by commas, '*' for
 * any text, '-' before a name to leave it out ("list_*, -list_secrets"). Empty: Homey's everyday tools for a
 * Homey, else every tool but those deleting or removing, at most MCP_DEFAULT_MAX_TOOLS.
 */

#pragma once

#include <stdbool.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where a server stands (MCP page) */
typedef enum {
    MCP_STATE_OFF,          /* switched off */
    MCP_STATE_WAITING,      /* not read yet (Wi-Fi not up, or being read) */
    MCP_STATE_READY,        /* its tools offered */
    MCP_STATE_UNREACHABLE,  /* not reachable or refused: tried again every minute */
} mcp_state_t;

/** Read the servers of CONFIG.INI (after settings_read_parameter) */
void mcp_init(void);

/** Number of servers configured */
int mcp_count(void);

/** Its name (McpN_name, else the host of its address; "Homey") */
const char *mcp_name(int index);

/** Where it stands, and the number of tools it offers */
mcp_state_t mcp_state(int index, int *tools);

/** Switched on or off (saved in McpN_on); applied by mcp_poll() */
void mcp_set_enabled(int index, bool on);

/**
 * @brief Called every second by the main task: reads the tools of the servers switched on and not read yet (Wi-Fi
 *        up; again every minute while one cannot be reached), and gives the model the new list of tools.
 *
 * @return true when what the MCP page shows changed (states, tools)
 */
bool mcp_poll(bool wifi_up);

/** A Homey is configured and switched on (its rules go into the instructions) */
bool mcp_homey_on(void);

/** The tool is one of an MCP server switched on */
bool mcp_has_tool(const char *name);

/**
 * @brief Call an MCP tool (by the name the model knows).
 *
 * @return Text result for the model (allocated, to be freed)
 */
char *mcp_call(const char *name, const cJSON *input, bool *is_error);

#ifdef __cplusplus
}
#endif
