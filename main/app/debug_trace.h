/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Debug traces (Debug = on in CONFIG.INI, or the Debug page of the Settings screen): every line of the logs, with
 * the time of day and the number of the exchange ("14:32:05.123 #12 I tts: ..."), kept in a ring buffer in PSRAM
 * (the oldest lines dropped), still printed on the USB serial port. Nothing is written to the flash while testing
 * (that would itself cause lags); the Dump button writes the buffer to /spiffs/debug.txt, shown as DEBUG.TXT on
 * the configuration drive. Off, the logs go to the serial port only, as before.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEBUG_TRACE_FILE    "/spiffs/debug.txt"

/** Start (the buffer allocated, the logs caught) or stop the traces; what was kept stays until the restart */
void debug_trace_set(bool on);

/** The traces are on */
bool debug_trace_on(void);

/** A new exchange (wake word, tap): its number goes up, a line gives the Wi-Fi signal, the memory and the volume */
void debug_trace_turn(const char *how);

/** A mark put by the user (long press on the screen, "note ça"): its number, also in the traces */
int debug_trace_mark(const char *how);

/** Lines and bytes kept */
void debug_trace_stats(size_t *lines, size_t *bytes);

/** The buffer written to DEBUG_TRACE_FILE (the newest lines when SPIFFS has not room for all); false on error */
bool debug_trace_dump(void);

/* A trace only written when the traces are on (details too many for the usual logs) */
#define TRACE(tag, fmt, ...) do { \
        if (debug_trace_on()) { \
            ESP_LOGI(tag, "[trace] " fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#ifdef __cplusplus
}
#endif
