/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "app_sr.h"
#include "esp_mn_speech_commands.h"
#include "esp_process_sdkconfig.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_models.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_mn_iface.h"
#include "model_path.h"
#include "bsp_board.h"
#include "app_audio.h"
#include "app_wifi.h"
#include "stt_stream.h"
#include "settings.h"

static const char *TAG = "app_sr";

/*
 * Listening ends after this many fetch frames of silence following speech. The VAD itself only
 * reports silence after End_speech_ms of CONFIG.INI (vad_min_noise_ms), so this adds little.
 */
#define SR_SILENCE_END_FRAMES       2
/* Without any speech, a listening session started by wake word or touch is dropped after this delay */
#define SR_NO_SPEECH_TIMEOUT_MS     8000
/*
 * VAD is not counted at the start of a session, while sounds of the device are still audible (no AEC):
 * the wake beep (0.5 s) after a wake word, the end of the spoken reply for a follow-up. The audio is
 * still streamed to the transcription: a short question said right after the wake word is not lost.
 */
#define SR_VAD_IGNORE_WAKE_MS       800
#define SR_VAD_IGNORE_FOLLOWUP_MS   1200
/* Cumulated speech required before a session is considered to contain speech (filters noise bursts) */
#define SR_MIN_SPEECH_MS            300
/*
 * VAD frames quieter than this are not counted as speech: the VAD keeps reporting speech for
 * about 1 s after the beep / spoken reply. Measured on BOX-3: voice at ~1 m about -45 dBFS,
 * room silence about -68 dBFS.
 */
#define SR_SPEECH_MIN_VOLUME_DBFS   (-58.0f)

static const esp_afe_sr_iface_t *afe_handle = NULL;
static srmodel_list_t *models = NULL;
static volatile bool manul_detect_flag = false;
/* Double tap while listening: the session ends as if nobody had spoken */
static volatile bool s_cancel_listen = false;
/* Non-zero: the pending manual session is a follow-up that must hear speech within this delay */
static volatile uint32_t s_followup_window_ms = 0;

sr_data_t *g_sr_data = NULL;

#define I2S_CHANNEL_NUM      2

extern bool record_flag;
extern uint32_t record_total_len;

static void audio_feed_task(void *arg)
{
    ESP_LOGI(TAG, "Feed Task");
    size_t bytes_read = 0;
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *) arg;
    int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int feed_channel = afe_handle->get_feed_channel_num(afe_data);
    ESP_LOGI(TAG, "audio_chunksize=%d, feed_channel=%d", audio_chunksize, feed_channel);
    /* The channel adjust below and audio_record_save() expect 3 interleaved channels */
    assert(feed_channel == 3);

    /* Allocate audio buffer and check for result */
    int16_t *audio_buffer = heap_caps_malloc(audio_chunksize * sizeof(int16_t) * feed_channel, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(audio_buffer);
    g_sr_data->afe_in_buffer = audio_buffer;

    while (true) {
        if (g_sr_data->event_group && xEventGroupGetBits(g_sr_data->event_group)) {
            xEventGroupSetBits(g_sr_data->event_group, FEED_DELETED);
            vTaskDelete(NULL);
        }

        /* Read audio data from I2S bus */
        bsp_i2s_read((char *)audio_buffer, audio_chunksize * I2S_CHANNEL_NUM * sizeof(int16_t), &bytes_read, portMAX_DELAY);

        /* Channel Adjust */
        for (int  i = audio_chunksize - 1; i >= 0; i--) {
            audio_buffer[i * 3 + 2] = 0;
            audio_buffer[i * 3 + 1] = audio_buffer[i * 2 + 1];
            audio_buffer[i * 3 + 0] = audio_buffer[i * 2 + 0];
        }

        /* Checking if WIFI is connected */
        if (WIFI_STATUS_CONNECTED_OK == wifi_connected_already()) {

            /* Feed samples of an audio stream to the AFE_SR */
            afe_handle->feed(afe_data, audio_buffer);
        }
        audio_record_save(audio_buffer, audio_chunksize);
    }
}

static void audio_detect_task(void *arg)
{
    ESP_LOGI(TAG, "Detection task");
    static vad_state_t local_state;
    static uint8_t frame_keep = 0;

    bool detect_flag = false;
    bool speech_seen = false;
    uint32_t speech_ms = 0;
    float max_volume = -100.0f;
    TickType_t listen_start = 0;
    uint32_t no_speech_timeout_ms = SR_NO_SPEECH_TIMEOUT_MS;
    uint32_t vad_ignore_ms = SR_VAD_IGNORE_WAKE_MS;
    esp_afe_sr_data_t *afe_data = arg;
    const uint32_t frame_ms = afe_handle->get_fetch_chunksize(afe_data) * 1000 / afe_handle->get_samp_rate(afe_data);
    ESP_LOGI(TAG, "fetch frame: %" PRIu32 " ms", frame_ms);

    while (true) {
        if (NEED_DELETE && xEventGroupGetBits(g_sr_data->event_group)) {
            xEventGroupSetBits(g_sr_data->event_group, DETECT_DELETED);
            vTaskDelete(g_sr_data->handle_task);
            vTaskDelete(NULL);
        }
        afe_fetch_result_t *res = afe_handle->fetch(afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            continue;
        }
        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG,  "wakeword detected (model %d)", res->wakenet_model_index);
            sr_result_t result = {
                .wakenet_mode = WAKENET_DETECTED,
                .state = ESP_MN_STATE_DETECTING,
                .command_id = 0,
            };
            xQueueSend(g_sr_data->result_que, &result, 0);
        } else if (res->wakeup_state == WAKENET_CHANNEL_VERIFIED || manul_detect_flag) {
            detect_flag = true;
            speech_seen = false;
            speech_ms = 0;
            max_volume = -100.0f;
            listen_start = xTaskGetTickCount();
            no_speech_timeout_ms = SR_NO_SPEECH_TIMEOUT_MS;
            vad_ignore_ms = SR_VAD_IGNORE_WAKE_MS;
            if (manul_detect_flag) {
                bool followup = (s_followup_window_ms > 0);
                if (followup) {
                    no_speech_timeout_ms = s_followup_window_ms;
                    vad_ignore_ms = SR_VAD_IGNORE_FOLLOWUP_MS;
                    s_followup_window_ms = 0;
                }
                manul_detect_flag = false;
                sr_result_t result = {
                    .wakenet_mode = WAKENET_DETECTED,
                    .state = ESP_MN_STATE_DETECTING,
                    .command_id = 0,
                    .followup = followup,
                };
                xQueueSend(g_sr_data->result_que, &result, 0);
            }
            frame_keep = 0;
            g_sr_data->afe_handle->disable_wakenet(afe_data);
            ESP_LOGI(TAG,  "AFE_FETCH_CHANNEL_VERIFIED, channel index: %d\n", res->trigger_channel_id);
        }

        if (true == detect_flag) {
            /* The cleaned-up AFE output (beamformed, 16 kHz mono) is streamed to the transcription */
            if (res->data && res->data_size > 0) {
                stt_stream_write(res->data, res->data_size / sizeof(int16_t));
            }

            if (local_state != res->vad_state) {
                local_state = res->vad_state;
                frame_keep = 0;
            } else {
                frame_keep++;
            }

            uint32_t elapsed_ms = pdTICKS_TO_MS(xTaskGetTickCount() - listen_start);
            if (elapsed_ms > vad_ignore_ms && res->data_volume > max_volume) {
                max_volume = res->data_volume;
            }
            if (!speech_seen && VAD_SPEECH == res->vad_state && elapsed_ms > vad_ignore_ms &&
                    res->data_volume > SR_SPEECH_MIN_VOLUME_DBFS) {
                speech_ms += frame_ms;
                if (speech_ms >= SR_MIN_SPEECH_MS) {
                    speech_seen = true;
                    ESP_LOGI(TAG, "speech detected after %" PRIu32 " ms (volume %.1f dBFS)", elapsed_ms, res->data_volume);
                }
            }

            bool no_speech = (!speech_seen && elapsed_ms >= no_speech_timeout_ms) || s_cancel_listen;
            if (s_cancel_listen) {
                s_cancel_listen = false;
                speech_seen = false;
                ESP_LOGI(TAG, "listening cancelled");
            }
            bool speech_ended = speech_seen && (SR_SILENCE_END_FRAMES == frame_keep) && (VAD_SILENCE == res->vad_state);
            if (no_speech || speech_ended) {
                ESP_LOGI(TAG, "listen end after %" PRIu32 " ms: speech %" PRIu32 " ms, max volume %.1f dBFS%s",
                         elapsed_ms, speech_ms, max_volume, no_speech ? " (no speech)" : "");
                sr_result_t result = {
                    .wakenet_mode = WAKENET_NO_DETECT,
                    .state = ESP_MN_STATE_TIMEOUT,
                    .command_id = 0,
                    .no_speech = no_speech,
                };
                xQueueSend(g_sr_data->result_que, &result, 0);
                g_sr_data->afe_handle->enable_wakenet(afe_data);
                detect_flag = false;
                continue;
            }
        }
    }
    /* Task never returns */
    vTaskDelete(NULL);
}

esp_err_t app_sr_set_language(sr_language_t new_lang)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR is not running");

    if (new_lang == g_sr_data->lang) {
        ESP_LOGW(TAG, "nothing to do");
        return ESP_OK;
    } else {
        g_sr_data->lang = new_lang;
    }
    ESP_LOGI(TAG, "Set language %s", SR_LANG_EN == g_sr_data->lang ? "EN" : "CN");
    /* Wake words are loaded once in app_sr_start(); they do not depend on this language */
    return ESP_OK;
}

esp_err_t app_sr_start(bool record_en)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(NULL == g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR already running");

    g_sr_data = heap_caps_calloc(1, sizeof(sr_data_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_NO_MEM, TAG, "Failed create sr data");

    g_sr_data->result_que = xQueueCreate(3, sizeof(sr_result_t));
    ESP_GOTO_ON_FALSE(NULL != g_sr_data->result_que, ESP_ERR_NO_MEM, err, TAG, "Failed create result queue");

    g_sr_data->event_group = xEventGroupCreate();
    ESP_GOTO_ON_FALSE(NULL != g_sr_data->event_group, ESP_ERR_NO_MEM, err, TAG, "Failed create event_group");

    BaseType_t ret_val;
    models = esp_srmodel_init("model");
    ESP_GOTO_ON_FALSE(NULL != models, ESP_FAIL, err, TAG, "Failed to load SR models");

    /* Feed format: 2 microphones + 1 unused channel (no playback reference, so no AEC) */
    afe_config_t *afe_config = afe_config_init("MMN", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    ESP_GOTO_ON_FALSE(NULL != afe_config, ESP_FAIL, err, TAG, "Failed to init afe config");
    afe_config->aec_init = false;
    /* End of speech: silence needed before the VAD reports it (End_speech_ms, ESP-SR default 1000 ms) */
    afe_config->vad_min_noise_ms = settings_get_parameter()->end_speech_ms;
    ESP_LOGI(TAG, "End of speech after %d ms of silence", afe_config->vad_min_noise_ms);

    /* Up to two wake words: French "Bonjour ESP" first, English "Hi ESP" as fallback */
    char *wn_name_1 = esp_srmodel_filter(models, ESP_WN_PREFIX, "bonjouresp");
    char *wn_name_2 = esp_srmodel_filter(models, ESP_WN_PREFIX, "hiesp");
    if (NULL == wn_name_1) {
        wn_name_1 = wn_name_2;
        wn_name_2 = NULL;
    }
    ESP_LOGI(TAG, "load wakenet: %s, %s", wn_name_1 ? wn_name_1 : "-", wn_name_2 ? wn_name_2 : "-");
    afe_config->wakenet_model_name = wn_name_1;
    afe_config->wakenet_model_name_2 = wn_name_2;

    afe_handle = esp_afe_handle_from_config(afe_config);
    esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);
    ESP_GOTO_ON_FALSE(NULL != afe_data, ESP_FAIL, err, TAG, "Failed to create afe");
    g_sr_data->afe_handle = afe_handle;
    g_sr_data->afe_data = afe_data;

    g_sr_data->lang = SR_LANG_MAX;
    ret = app_sr_set_language(SR_LANG_EN);
    ESP_GOTO_ON_FALSE(ESP_OK == ret, ESP_FAIL, err, TAG,  "Failed to set language");

    /* Stack sizes measured with uxTaskGetSystemState(), plus a 2 KB margin: internal RAM is scarce */
    ret_val = xTaskCreatePinnedToCore(&audio_feed_task, "Feed Task", 3072, (void *)afe_data, 5, &g_sr_data->feed_task, 0);
    ESP_GOTO_ON_FALSE(pdPASS == ret_val, ESP_FAIL, err, TAG,  "Failed create audio feed task");

    ret_val = xTaskCreatePinnedToCore(&audio_detect_task, "Detect Task", 3584, (void *)afe_data, 5, &g_sr_data->detect_task, 1);
    ESP_GOTO_ON_FALSE(pdPASS == ret_val, ESP_FAIL, err, TAG,  "Failed create audio detect task");

    ret_val = xTaskCreatePinnedToCore(&sr_handler_task, "SR Handler Task", 6656, NULL, 5, &g_sr_data->handle_task, 0);
    ESP_GOTO_ON_FALSE(pdPASS == ret_val, ESP_FAIL, err, TAG,  "Failed create audio handler task");

    audio_record_init();

    return ESP_OK;
err:
    app_sr_stop();
    return ret;
}

esp_err_t app_sr_stop(void)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR is not running");
    xEventGroupSetBits(g_sr_data->event_group, NEED_DELETE);
    xEventGroupWaitBits(g_sr_data->event_group, NEED_DELETE | FEED_DELETED | DETECT_DELETED | HANDLE_DELETED, 1, 1, portMAX_DELAY);

    if (g_sr_data->result_que) {
        vQueueDelete(g_sr_data->result_que);
        g_sr_data->result_que = NULL;
    }

    if (g_sr_data->event_group) {
        vEventGroupDelete(g_sr_data->event_group);
        g_sr_data->event_group = NULL;
    }

    if (g_sr_data->fp) {
        fclose(g_sr_data->fp);
        g_sr_data->fp = NULL;
    }

    if (g_sr_data->model_data) {
        g_sr_data->multinet->destroy(g_sr_data->model_data);
    }

    if (g_sr_data->afe_data) {
        g_sr_data->afe_handle->destroy(g_sr_data->afe_data);
    }

    if (g_sr_data->afe_in_buffer) {
        heap_caps_free(g_sr_data->afe_in_buffer);
    }

    if (g_sr_data->afe_out_buffer) {
        heap_caps_free(g_sr_data->afe_out_buffer);
    }

    heap_caps_free(g_sr_data);
    g_sr_data = NULL;
    return ESP_OK;
}

esp_err_t app_sr_get_result(sr_result_t *result, TickType_t xTicksToWait)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR is not running");
    xQueueReceive(g_sr_data->result_que, result, xTicksToWait);
    return ESP_OK;
}

void app_sr_cancel(void)
{
    s_cancel_listen = true;
}

esp_err_t app_sr_start_once(void)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR is not running");
    s_followup_window_ms = 0;
    manul_detect_flag = true;
    return ESP_OK;
}

esp_err_t app_sr_start_followup(uint32_t window_ms)
{
    ESP_RETURN_ON_FALSE(NULL != g_sr_data, ESP_ERR_INVALID_STATE, TAG, "SR is not running");
    ESP_RETURN_ON_FALSE(window_ms > 0, ESP_ERR_INVALID_ARG, TAG, "Follow-up window must be > 0");
    s_followup_window_ms = window_ms;
    manul_detect_flag = true;
    return ESP_OK;
}
