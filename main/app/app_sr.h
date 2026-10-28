/*
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <sys/queue.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_models.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NEED_DELETE          BIT0
#define FEED_DELETED         BIT1
#define DETECT_DELETED       BIT2
#define HANDLE_DELETED       BIT3

#define SR_CONTINUE_DET 1
#define SR_RUN_TEST 0 /**< Just for sr experiment in laboratory >*/
#if SR_RUN_TEST
#ifdef SR_CONTINUE_DET
#undef SR_CONTINUE_DET
#define SR_CONTINUE_DET 0
#endif
#endif

typedef struct {
    wakenet_state_t wakenet_mode;
    esp_mn_state_t state;
    int command_id;
    bool followup;   /* WAKENET_DETECTED: session continues the conversation (no wake word) */
    bool no_speech;  /* ESP_MN_STATE_TIMEOUT: nobody spoke, nothing to send */
    bool listen_panel_later;    /* WAKENET_DETECTED: the reply stays shown (text to read), show_listen comes later */
    bool show_listen;           /* listening for a while already: time to show the listen panel (speech heard, or
                                   the reading time is over) */
} sr_result_t;

typedef enum {
    SR_LANG_EN,
    SR_LANG_CN,
    SR_LANG_MAX,
} sr_language_t;


typedef struct {
    sr_language_t lang;
    model_iface_data_t *model_data;
    const esp_mn_iface_t *multinet;
    const esp_afe_sr_iface_t *afe_handle;
    esp_afe_sr_data_t *afe_data;
    int16_t *afe_in_buffer;
    int16_t *afe_out_buffer;
    uint8_t cmd_num;
    TaskHandle_t feed_task;
    TaskHandle_t detect_task;
    TaskHandle_t handle_task;
    QueueHandle_t result_que;
    EventGroupHandle_t event_group;
    FILE *fp;
    bool b_record_en;
} sr_data_t;

esp_err_t app_sr_start(bool record_en);
esp_err_t app_sr_stop(void);
esp_err_t app_sr_get_result(sr_result_t *result, TickType_t xTicksToWait);
/** The listening session ends as if nobody had spoken (double tap while "J'écoute") */
void app_sr_cancel(void);

esp_err_t app_sr_start_once(void);

/**
 * @brief Listen again without wake word to continue the conversation.
 *
 * The session is dropped (no request sent) if no speech is heard within window_ms. With panel_delay_ms, the
 * listening starts at once but the reply stays shown that long (a list shown, not read, is being read): the listen
 * panel comes after it, or as soon as speech is heard.
 */
esp_err_t app_sr_start_followup(uint32_t window_ms, uint32_t panel_delay_ms);

#ifdef __cplusplus
}
#endif
