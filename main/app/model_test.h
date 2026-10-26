/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * "Teste les modèles": the same question asked to every model offered (AGENTS.INI, then the free OpenRouter
 * ones), one after the other, without the conversation nor tools. The times are kept in LOG.TXT (configuration
 * drive) and the speed of the models of AGENTS.INI (fast, average, slow) is written to that file.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Ask for the test (nothing if one is running); it is run by model_test_poll() */
void model_test_start(void);

/**
 * @brief Run the test if asked: from the main task's loop, whose stack (in internal RAM, needed by the TLS
 *        handshakes) is free meanwhile. Blocking for several minutes; each result is spoken as it comes.
 */
void model_test_poll(void);

/** A test is running */
bool model_test_running(void);

/** Stop the test after the model being tested (a new listening session started): nothing more is spoken */
void model_test_cancel(void);

/** The running test was stopped */
bool model_test_cancelled(void);

#ifdef __cplusplus
}
#endif
