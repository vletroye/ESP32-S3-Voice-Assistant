/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * "Teste les modèles": the same question asked to every model offered (AGENTS.INI, then the free OpenRouter
 * ones), one after the other, without the conversation nor tools; or only to those in use, or to those of a
 * provider (model_test_scope_t). The times are kept in SPEED.TXT (configuration
 * drive) and the speed of the models of AGENTS.INI (fast, average, slow) is written to that file.
 */

#pragma once

#include "cJSON.h"
#include <stdbool.h>
#include "llm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The models of a test */
typedef enum {
    MODEL_TEST_ALL,         /* "teste les modèles": the speech models, then every model offered */
    MODEL_TEST_CURRENT,     /* "teste les modèles en cours": the transcription, speech and chat models in use */
    MODEL_TEST_PROVIDER,    /* "teste les modèles d'Anthropic": the chat models of a provider */
    MODEL_TEST_TTS,         /* "teste les modèles de TTS": the speech models */
    MODEL_TEST_STT,         /* "teste les modèles de STT": the transcription models (given the audio of a speech one) */
    MODEL_TEST_LLM,         /* "teste les modèles de LLM": the chat models */
    MODEL_TEST_MODEL,       /* "teste le modèle Argon": one chat model (model_test_set_model) */
} model_test_scope_t;

/** The models of the next test (provider: for MODEL_TEST_PROVIDER), kept until it is started */
void model_test_set_scope(model_test_scope_t scope, llm_provider_t provider);

/** The next test: this chat model only (its id, copied) */
void model_test_set_model(const char *id);

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

/**
 * @brief After the test, the free OpenRouter models slower than MODEL_TEST_SLOW_FREE_MS on average are asked about
 *        one by one: the question about the next one ("Le modèle X fait 7,2 secondes en moyenne. Dois-je
 *        l'exclure ?") into buf; false when none is left.
 */
bool model_test_exclusion_question(char *buf, size_t size);

/** The answer to that question: yes excludes the model (AGENTS.INI), then the next one is asked */
void model_test_exclusion_answer(bool yes);

/** No more questions (another sentence was said) */
void model_test_exclusions_drop(void);

/**
 * @brief The 3 fastest models offered according to the tests (mean time to the first words), as said:
 *        "Haiku (0,8 seconde), Flash-Lite (1,1 seconde) et Luna (1,4 seconde)"; false if none was tested yet
 */
bool model_test_fastest(char *buf, size_t size);

/** The results of the last tests, for model_test_result (to be freed with cJSON_Delete) */
cJSON *model_test_results(void);

/**
 * @brief " (1,2 s)": the time to the first words (first audio, transcript) of a model's last test (key: its id; a
 *        speech model: "tts:<id>", "stt:<id>"), " (indisponible)" if it did not answer, "" if never tested
 */
void model_test_result(const cJSON *results, const char *key, char *buf, size_t size);

#ifdef __cplusplus
}
#endif
