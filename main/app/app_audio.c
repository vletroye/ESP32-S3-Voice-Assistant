/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#include "esp_vfs.h"
#include "app_sr.h"
#include "app_presence.h"
#include "app_audio.h"
#include "settings.h"
#include "debug_trace.h"
#include "bsp_board.h"
#include "bsp/esp-bsp.h"
#include "app_ui_ctrl.h"
#include "app_wifi.h"
#include "stt_stream.h"
#include "tts_stream.h"
#include "esp_heap_caps.h"
#include "radio.h"
#include "llm.h"
#include "alarm_ring.h"
#include "game.h"

static const char *TAG = "app_audio";

#if !CONFIG_BSP_BOARD_ESP32_S3_BOX_Lite
static bool mute_flag = true;
#endif
bool record_flag = false;
uint32_t record_total_len = 0;
uint32_t file_total_len = 0;
static uint8_t *record_audio_buffer = NULL;
uint8_t *audio_rx_buffer = NULL;

extern sr_data_t *g_sr_data;
extern void assistant_listen_start(void);
extern void assistant_listen_end(void);
extern void assistant_listen_abort(void);
extern void assistant_prewarm(void);
extern bool assistant_resume_ask(void);
extern bool assistant_question_ask(void);
extern void assistant_answer_tapped(int number, bool was_listening);
extern void assistant_reply_stop(void);
extern int Cache_WriteBack_Addr(uint32_t addr, uint32_t size);

/* main function */
void mute_btn_handler(void *handle, void *arg)
{
#if !CONFIG_BSP_BOARD_ESP32_S3_BOX_Lite
    button_event_t event = (button_event_t)arg;

    if (BUTTON_PRESS_DOWN == event) {
        esp_rom_printf(DRAM_STR("Audio Mute On\r\n"));
        mute_flag = true;
    } else {
        esp_rom_printf(DRAM_STR("Audio Mute Off\r\n"));
        mute_flag = false;
    }
#endif
}


void audio_record_init()
{
    /* Create file if record to SD card enabled*/
#if DEBUG_SAVE_PCM
    record_audio_buffer = heap_caps_calloc(1, FILE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(record_audio_buffer);
    printf("successfully created record_audio_buffer with a size: %zu\n", FILE_SIZE);
    audio_rx_buffer = heap_caps_calloc(1, MAX_FILE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(audio_rx_buffer);
    printf("audio_rx_buffer with a size: %zu\n", MAX_FILE_SIZE);
#endif

#if DEBUG_SAVE_PCM
    if (record_audio_buffer == NULL || audio_rx_buffer == NULL) {
        printf("Error: Failed to allocate memory for buffers\n");
        return; // Return or handle the error condition appropriately
    }
#endif

    /* Spoken replies are played by tts_stream, the short system sounds by audio_play_task() */
    audio_play_preload("/spiffs/echo_en_wake.wav");
}

void audio_record_save(int16_t *audio_buffer, int audio_chunksize)
{
#if DEBUG_SAVE_PCM
    if (record_flag) {
        uint16_t *record_buff = (uint16_t *)(record_audio_buffer + sizeof(wav_header_t));
        record_buff += record_total_len;
        for (int i = 0; i < (audio_chunksize - 1); i++) {
            // Bounded by the record buffer size (FILE_SIZE), not MAX_FILE_SIZE
            if (record_total_len < (FILE_SIZE - sizeof(wav_header_t)) / sizeof(uint16_t)) {
#if PCM_ONE_CHANNEL
                record_buff[ i * 1 + 0] = audio_buffer[i * 3 + 0];
                record_total_len += 1;
#else
                record_buff[ i * 2 + 0] = audio_buffer[i * 3 + 0];
                record_buff[ i * 2 + 1] = audio_buffer[i * 3 + 1];
                record_total_len += 2;
#endif
            }
        }
    }
#endif
}

static void audio_record_start()
{
    // record_flag marks a listening session (the reply callbacks check it), recording or not
    record_total_len = 0;
    record_flag = true;
#if DEBUG_SAVE_PCM
    ESP_LOGI(TAG, "### record Start");
    file_total_len = sizeof(wav_header_t);
#endif
}

static esp_err_t audio_record_stop()
{
    esp_err_t ret = ESP_OK;
    record_flag = false;
#if DEBUG_SAVE_PCM
    FILE *fp = NULL;
#if PCM_ONE_CHANNEL
    record_total_len *= 1;
#else
    record_total_len *= 2;
#endif
    file_total_len += record_total_len;
    ESP_LOGI(TAG, "### record Stop, %" PRIu32 " %" PRIu32 "K", \
             record_total_len, \
             record_total_len / 1024);

    fp = fopen("/spiffs/echo_en_wake.wav", "r");
    ESP_GOTO_ON_FALSE(NULL != fp, ESP_FAIL, err, TAG, "Failed create record file");

    wav_header_t wav_head;
    int len = fread(&wav_head, 1, sizeof(wav_header_t), fp);
    ESP_GOTO_ON_FALSE(len > 0, ESP_FAIL, err, TAG, "Failed create record file");

    wav_head.SampleRate = 16000;
#if PCM_ONE_CHANNEL
    wav_head.NumChannels = 1;
#else
    wav_head.NumChannels = 2;
#endif
    wav_head.BitsPerSample = 16;
    wav_head.ChunkSize = file_total_len - 8;
    wav_head.ByteRate = wav_head.SampleRate * wav_head.BitsPerSample * wav_head.NumChannels / 8;
    wav_head.Subchunk2ID[0] = 'd';
    wav_head.Subchunk2ID[1] = 'a';
    wav_head.Subchunk2ID[2] = 't';
    wav_head.Subchunk2ID[3] = 'a';
    wav_head.Subchunk2Size = record_total_len;
    memcpy((void *)record_audio_buffer, &wav_head, sizeof(wav_header_t));
    Cache_WriteBack_Addr((uint32_t)record_audio_buffer, record_total_len);

err:
    if (fp) {
        fclose(fp);
    }
#endif
    return ret;
}

/*
 * A sound being spoken must have left the speaker before a system sound starts (two writers would mix). The I2S
 * DMA still holds ~96 ms of it once its last block is written: the beep waits for that, plus a short silence
 * (right after the last syllable, it crackled)
 */
#define SOUND_WAIT_SILENCE_MS   150
#define SOUND_WAIT_MAX_MS       600
/* With Followup_beep: the beep this long after the last word of the reply */
#define FOLLOWUP_BEEP_PAUSE_MS  700
/* A system sound written in pieces of this length; the I2S DMA holds about SOUND_DMA_MS of audio (6 x 240 frames) */
#define SOUND_PIECE_MS          20
#define SOUND_DMA_MS            80

/* System sound loaded once in PSRAM as 16 kHz stereo samples: played in one write, without file reads */
typedef struct {
    char path[48];
    int16_t *stereo;
    size_t bytes;
} sound_t;

static sound_t s_sound;

/*
 * Load a short system sound (WAV, PCM 16-bit, 16 kHz, mono or stereo). Only the "data" chunk is kept
 * (metadata chunks such as LIST or C2PA are skipped). Other sample rates are refused: the I2S bus is
 * shared with the microphones and must stay at 16 kHz.
 */
static esp_err_t sound_load(const char *filepath, sound_t *sound)
{
    esp_err_t ret = ESP_OK;
    uint8_t *in = NULL;

    FILE *fp = fopen(filepath, "rb");
    ESP_GOTO_ON_FALSE(NULL != fp, ESP_FAIL, EXIT, TAG, "Failed to open %s", filepath);

    uint8_t riff[12];
    ESP_GOTO_ON_FALSE(fread(riff, 1, sizeof(riff), fp) == sizeof(riff) && memcmp(riff, "RIFF", 4) == 0 &&
                      memcmp(riff + 8, "WAVE", 4) == 0, ESP_FAIL, EXIT, TAG, "%s: not a WAV file", filepath);

    /* Walk the chunks: "fmt " gives the format, "data" the samples */
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0, data_size = 0;
    bool has_fmt = false;
    for (;;) {
        uint8_t chunk[8];
        ESP_GOTO_ON_FALSE(fread(chunk, 1, sizeof(chunk), fp) == sizeof(chunk), ESP_FAIL, EXIT, TAG,
                          "%s: no data chunk", filepath);
        uint32_t size = chunk[4] | (chunk[5] << 8) | (chunk[6] << 16) | ((uint32_t)chunk[7] << 24);
        if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
            uint8_t f[16];
            ESP_GOTO_ON_FALSE(fread(f, 1, sizeof(f), fp) == sizeof(f), ESP_FAIL, EXIT, TAG, "Bad fmt chunk");
            format = f[0] | (f[1] << 8);
            channels = f[2] | (f[3] << 8);
            rate = f[4] | (f[5] << 8) | (f[6] << 16) | ((uint32_t)f[7] << 24);
            bits = f[14] | (f[15] << 8);
            has_fmt = true;
            size -= sizeof(f);
        } else if (memcmp(chunk, "data", 4) == 0) {
            data_size = size;
            break;
        }
        fseek(fp, size + (size & 1), SEEK_CUR);
    }
    ESP_GOTO_ON_FALSE(has_fmt && format == 1 && bits == 16 && rate == 16000 && (channels == 1 || channels == 2),
                      ESP_ERR_NOT_SUPPORTED, EXIT, TAG, "%s: only 16 kHz 16-bit PCM WAV files are supported", filepath);

    size_t frames = data_size / (channels * sizeof(int16_t));
    in = heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    sound->stereo = heap_caps_malloc(frames * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_GOTO_ON_FALSE(in && sound->stereo, ESP_ERR_NO_MEM, EXIT, TAG, "Out of memory");
    frames = fread(in, 1, data_size, fp) / (channels * sizeof(int16_t));
    const int16_t *samples = (const int16_t *)in;
    for (size_t i = 0; i < frames; i++) {
        sound->stereo[2 * i] = samples[i * channels];
        sound->stereo[2 * i + 1] = samples[i * channels + (channels - 1)];
    }
    sound->bytes = frames * 2 * sizeof(int16_t);
    strlcpy(sound->path, filepath, sizeof(sound->path));
    ESP_LOGI(TAG, "%s: %u ms loaded", filepath, (unsigned)(frames * 1000 / 16000));

EXIT:
    if (fp) {
        fclose(fp);
    }
    free(in);
    if (ret != ESP_OK) {
        free(sound->stereo);
        sound->stereo = NULL;
        sound->bytes = 0;
    }
    return ret;
}

esp_err_t audio_play_preload(const char *filepath)
{
    if (s_sound.stereo && strcmp(s_sound.path, filepath) == 0) {
        return ESP_OK;
    }
    free(s_sound.stereo);
    memset(&s_sound, 0, sizeof(s_sound));
    return sound_load(filepath, &s_sound);
}

/* Play a short system sound from memory (loaded by audio_play_preload(), or here at the first call) */
esp_err_t audio_play_task(void *filepath)
{
    return audio_play_after_silence((const char *)filepath, SOUND_WAIT_SILENCE_MS);
}

esp_err_t audio_play_after_silence(const char *filepath, uint32_t silence_ms)
{
    ESP_RETURN_ON_ERROR(audio_play_preload(filepath), TAG, "Cannot play %s", filepath);
    /* No codec register write here: done once at boot (writing while sound plays out makes crackles) */
    int64_t t0 = esp_timer_get_time();
    while (!tts_stream_is_silent(silence_ms) && esp_timer_get_time() - t0 < (silence_ms + SOUND_WAIT_MAX_MS) * 1000LL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    int64_t waited_us = esp_timer_get_time() - t0;
    // Written 20 ms at a time: a piece written long after the previous one means the speaker ran dry meanwhile (the
    // I2S DMA holds SOUND_DMA_MS of audio), heard as a crackle
    size_t written = 0;
    const size_t piece = 16 * SOUND_PIECE_MS * 2 * sizeof(int16_t);
    int64_t start_us = esp_timer_get_time(), last_us = start_us, gap_us = 0;
    for (size_t off = 0; off < s_sound.bytes; off += piece) {
        size_t n = s_sound.bytes - off < piece ? s_sound.bytes - off : piece;
        bsp_i2s_write(s_sound.stereo + off / sizeof(int16_t), n, &written, portMAX_DELAY);
        int64_t now = esp_timer_get_time();
        gap_us = now - last_us > gap_us ? now - last_us : gap_us;
        last_us = now;
    }
    TRACE(TAG, "sound %s: %lld ms waited for silence, %u ms of sound written in %lld ms, longest wait between two "
          "pieces %lld ms%s", (const char *)filepath, waited_us / 1000, (unsigned)(s_sound.bytes / 64),
          (last_us - start_us) / 1000, gap_us / 1000,
          gap_us > SOUND_DMA_MS * 1000LL ? ": the speaker ran dry (crackle)" : "");
    return ESP_OK;
}

void sr_handler_task(void *pvParam)
{
#if !CONFIG_BSP_BOARD_ESP32_S3_BOX_Lite
    static bool mute_state = false;
    mute_flag = gpio_get_level(BSP_BUTTON_MUTE_IO);
    printf("sr handle task, mute:%d\n", mute_flag);
#endif

    while (true) {
        if (NEED_DELETE && xEventGroupGetBits(g_sr_data->event_group)) {
            xEventGroupSetBits(g_sr_data->event_group, HANDLE_DELETED);
            vTaskDelete(NULL);
        }

        sr_result_t result = {
            .wakenet_mode = WAKENET_NO_DETECT,
            .state = ESP_MN_STATE_DETECTING,
        };

        app_sr_get_result(&result, pdMS_TO_TICKS(1 * 1000));

#if !CONFIG_BSP_BOARD_ESP32_S3_BOX_Lite
        if (mute_state != mute_flag) {
            mute_state = mute_flag;
            if (false == mute_state) {
                bsp_codec_set_fs(16000, 16, 2);
                // The codec was reopened: set its output again
                bsp_codec_mute_set(false);
                bsp_codec_volume_set(CONFIG_VOLUME_LEVEL, NULL);
            }
        }
#endif
        if (result.tapped > 0) {
            // An item of the list tapped: as if its number had been said
            app_presence_activity();
            assistant_reply_stop();
            audio_record_stop();
            assistant_answer_tapped(result.tapped, result.was_listening);
            continue;
        }

        if (ESP_MN_STATE_TIMEOUT == result.state && result.no_speech && stt_stream_has_text() && !llm_cancelled()) {
            // The local VAD missed it (e.g. a very short question), but the transcription heard words
            ESP_LOGI(TAG, "No speech detected locally, but the transcription has text: answering");
            result.no_speech = false;
        }

        if (ESP_MN_STATE_TIMEOUT == result.state && result.no_speech) {
            ESP_LOGI(TAG, "No speech, back to sleep");
            assistant_listen_abort();
            audio_record_stop();
            ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, 0);
            continue;
        }

        if (ESP_MN_STATE_TIMEOUT == result.state) {
            ESP_LOGI(TAG, "ESP_MN_STATE_TIMEOUT");
            audio_record_stop();
            // No "wait please" sound: the screen shows the progress and the answer starts quickly
            if (WIFI_STATUS_CONNECTED_OK == wifi_connected_already()) {
                assistant_listen_end();
            } else {
                assistant_listen_abort();
            }
            continue;
        }

        if (WAKENET_DETECTED == result.wakenet_mode) {
            // The radio stops while the assistant listens and answers (the speaker is needed for the beep and the
            // answer); it plays again when the assistant goes back to sleep
            if (radio_playing()) {
                ESP_LOGI(TAG, "Listening over the radio");
            }
            radio_suspend();
            // Someone speaks to the assistant: the screen goes on if it was off
            app_presence_activity();
            // A timer or an alarm ringing stops (the user may then say "encore 5 minutes")
            // A game between two BOX stops, on both
            if (game_stop()) {
                ESP_LOGI(TAG, "Game stopped by the wake word or a touch");
            }
            if (alarm_ring_stop()) {
                ESP_LOGI(TAG, "Ringing stopped by the wake word or a touch");
            }
            // A new question interrupts the reply being spoken
            assistant_reply_stop();
            // Woken up (not a follow-up) after a pause in a conversation: asked first whether to go on with it (the
            // listening after that question is a follow-up: asked again, it looped)
            if (!result.followup && assistant_resume_ask()) {
                continue;
            }
            // Woken up (not a follow-up) with a question unanswered: asked again first
            if (!result.followup && assistant_question_ask()) {
                continue;
            }
            audio_record_start();
            // Connect to the transcription service while the user speaks (the audio is buffered meanwhile). Woken
            // by the wake word: after the beep (opening the connection while it played made it crackle each time)
            if (result.followup) {
                assistant_listen_start();
            }

            // UI show listen, or the reply kept shown (a list to read, a question asked: the user answers it) with
            // the moving bars beside the robot head
            if (result.keep_reply) {
                ui_ctrl_reply_listening(true);
            } else {
                ui_ctrl_guide_jump();
                ui_ctrl_show_panel(UI_CTRL_PANEL_LISTEN, 0);
            }

            // The beep: after the wake word (or a tap); listening again after a reply, only with Followup_beep, and
            // after a pause (right after the last word it spoiled the end of the reply)
            if (!result.followup) {
                audio_play_task("/spiffs/echo_en_wake.wav");
                assistant_listen_start();
            } else if (settings_get_parameter()->followup_beep) {
                audio_play_after_silence("/spiffs/echo_en_wake.wav", FOLLOWUP_BEEP_PAUSE_MS);
            }
            // Open the Claude and speech connections while the user speaks
            assistant_prewarm();
            continue;
        }
    }
    vTaskDelete(NULL);
}
