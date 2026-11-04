/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * The files written often by the assistant (the conversation, the costs, the results of the model tests, the
 * places of the weather) on their own LittleFS partition, "state", mounted on STATE_DIR: SPIFFS took 2 to 9 s to
 * write a few KB (everything stopping meanwhile), LittleFS a fraction of a second. The files of the configuration
 * drive stay on SPIFFS.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STATE_DIR   "/state"

/**
 * @brief Mount the partition (formatted the first time), then move there the files an older firmware kept on
 *        SPIFFS. After bsp_spiffs_mount().
 */
esp_err_t state_mount(void);

/** The factory reset: every file of the partition removed */
void state_erase(void);

/** A start: a line "2026-11-04 20:37:17  2.24.2  crash (panic)" added to boots.txt, the last STATE_BOOTS_MAX kept */
#define STATE_BOOTS_MAX     50
void state_boot_record(const char *version, const char *reason);

/** On the way to the configuration mode: boots.txt copied to SPIFFS, shown there as BOOTS.TXT */
void state_boots_export(void);

#ifdef __cplusplus
}
#endif
