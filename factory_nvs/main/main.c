/* SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_tinyuf2.h"
#include "ui.h"
#include "bsp/esp-bsp.h"
#include "esp_ota_ops.h"

#define NVS_MODIFIED_BIT          BIT0
#define SSID_SIZE 32
#define PASSWORD_SIZE 64
#define KEY_SIZE 256
#define URL_SIZE 64
#define LANG_SIZE 8
#define MODEL_SIZE 48


static const char *TAG = "ChatGPT_NVS";

static EventGroupHandle_t s_event_group;
static nvs_handle_t my_handle;
static size_t buf_len_long;

LV_FONT_DECLARE(ui_font_Latin14);
LV_FONT_DECLARE(ui_font_Latin16);
LV_FONT_DECLARE(ui_font_Latin20);

/* Place the Restart button, and override the texts of the SquareLine-generated screen (English) for supported languages */
static void ui_localize(const char *lang)
{
    bsp_display_lock(0);
    // Restart button at the bottom right, as on the assistant's screens
    lv_obj_set_align(ui_ButtonSetup, LV_ALIGN_BOTTOM_RIGHT);
    lv_obj_set_pos(ui_ButtonSetup, -16, -16);
    if (strcasecmp(lang, "fr") != 0) {
        bsp_display_unlock();
        return;
    }

    lv_obj_set_style_text_font(ui_LabelSetupStepTile, &ui_font_Latin20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(ui_LabelSetupStepTile, "Configuration");
    lv_obj_set_style_text_font(ui_LabelSetupStepContent, &ui_font_Latin14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(ui_LabelSetupStepContent,
                      "1. Ouvrez le lecteur « ESP-Box ».\n"
                      "2. Editez les fichiers de config.\n"
                      "3. Enregistrez les changements.\n"
                      "4. Attendez 5 secondes.\n"
                      "5. Appuyez sur « Redémarrer ».");
    lv_obj_set_style_text_font(ui_LabelSetupBtn, &ui_font_Latin16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(ui_LabelSetupBtn, "Redémarrer");
    // The generated button (72 px) is sized for "Restart"
    lv_obj_set_width(ui_ButtonSetup, 120);
    bsp_display_unlock();
}

/* Create a key with its default value so that it shows up in CONFIG.INI (an empty value is kept) */
static void nvs_init_str_if_missing(const char *key, const char *default_value)
{
    size_t len = 0;
    if (nvs_get_str(my_handle, key, NULL, &len) == ESP_OK) {
        return;
    }
    ESP_ERROR_CHECK(nvs_set_str(my_handle, key, default_value));
    ESP_ERROR_CHECK(nvs_commit(my_handle));
    ESP_LOGI(TAG, "no %s, give a init value to nvs", key);
}

static void uf2_nvs_modified_cb()
{
    ESP_LOGI(TAG, "uf2 nvs modified");
    xEventGroupSetBits(s_event_group, NVS_MODIFIED_BIT);
}

void app_main(void)
{
    const esp_partition_t *update_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    ESP_LOGI(TAG, "Switch to partition factory");
    esp_ota_set_boot_partition(update_partition);
    esp_err_t err = ESP_OK;

    const char *uf2_nvs_partition = "nvs";
    const char *uf2_nvs_namespace = "configuration";

    char ssid[SSID_SIZE] = {0};
    char password[PASSWORD_SIZE] = {0};
    char key[KEY_SIZE] = {0};
    char url[URL_SIZE] = {0};
    char lang[LANG_SIZE] = {0};

    s_event_group = xEventGroupCreate();

    // Initialize NVS
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = nvs_open_from_partition(uf2_nvs_partition, uf2_nvs_namespace, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } else {
        buf_len_long = sizeof(ssid);
        err = nvs_get_str(my_handle, "ssid", ssid, &buf_len_long);
        if (err != ESP_OK || buf_len_long == 0) {
            ESP_ERROR_CHECK(nvs_set_str(my_handle, "ssid", ""));
            ESP_ERROR_CHECK(nvs_commit(my_handle));
            ESP_LOGI(TAG, "no ssid, give a init value to nvs");
        } else {
            ESP_LOGI(TAG, "stored ssid:%s", ssid);
        }

        buf_len_long = sizeof(password);
        err = nvs_get_str(my_handle, "password", password, &buf_len_long);
        if (err != ESP_OK || buf_len_long == 0) {
            ESP_ERROR_CHECK(nvs_set_str(my_handle, "password", ""));
            ESP_ERROR_CHECK(nvs_commit(my_handle));
            ESP_LOGI(TAG, "no password, give a init value to nvs");
        } else {
            ESP_LOGI(TAG, "stored password: (%d chars)", (int)strlen(password));
        }

        buf_len_long = sizeof(key);
        err = nvs_get_str(my_handle, "ChatGPT_key", key, &buf_len_long);
        if (err != ESP_OK || buf_len_long == 0) {
            ESP_ERROR_CHECK(nvs_set_str(my_handle, "ChatGPT_key", ""));
            ESP_ERROR_CHECK(nvs_commit(my_handle));
            ESP_LOGI(TAG, "no ChatGPT key, give a init value to key");
        } else {
            ESP_LOGI(TAG, "stored ChatGPT key: (%d chars)", (int)strlen(key));
        }

        buf_len_long = sizeof(url);
        err = nvs_get_str(my_handle, "Base_url", url, &buf_len_long);
        if (err != ESP_OK || buf_len_long == 0) {
            ESP_ERROR_CHECK(nvs_set_str(my_handle, "Base_url", CONFIG_ASSISTANT_OPENAI_URL));
            ESP_ERROR_CHECK(nvs_commit(my_handle));
            ESP_LOGI(TAG, "no base url, give a init value to key");
        } else {
            ESP_LOGI(TAG, "stored base url:%s", url);
        }

        buf_len_long = sizeof(lang);
        err = nvs_get_str(my_handle, "Language", lang, &buf_len_long);
        if (err != ESP_OK || buf_len_long <= 1) {
            ESP_ERROR_CHECK(nvs_set_str(my_handle, "Language", CONFIG_ASSISTANT_LANGUAGE));
            ESP_ERROR_CHECK(nvs_commit(my_handle));
            strlcpy(lang, CONFIG_ASSISTANT_LANGUAGE, sizeof(lang));
            ESP_LOGI(TAG, "no language, give a init value to nvs");
        } else {
            ESP_LOGI(TAG, "stored language:%s", lang);
        }

        // Settings of former versions (OpenAI chat model, provider): no longer used
        bool erased = (nvs_erase_key(my_handle, "Model") == ESP_OK);
        erased |= (nvs_erase_key(my_handle, "Provider") == ESP_OK);
        // Trace of a debugging session of the configuration mode
        erased |= (nvs_erase_key(my_handle, "Uf2_trace") == ESP_OK);
        // OpenRouter models: now in AGENTS.INI
        erased |= (nvs_erase_key(my_handle, "OpenRouter_list") == ESP_OK);
        if (erased) {
            ESP_ERROR_CHECK(nvs_commit(my_handle));
            ESP_LOGI(TAG, "removed obsolete keys Model / Provider");
        }
        // "Claude_model" became "Chat_model" when the GPT models were added: keep the chosen model
        char model[MODEL_SIZE] = {0};
        buf_len_long = sizeof(model);
        if (nvs_get_str(my_handle, "Chat_model", NULL, &buf_len_long) != ESP_OK) {
            buf_len_long = sizeof(model);
            if (nvs_get_str(my_handle, "Claude_model", model, &buf_len_long) != ESP_OK || buf_len_long <= 1) {
                strlcpy(model, CONFIG_ASSISTANT_CHAT_MODEL, sizeof(model));
            }
            ESP_ERROR_CHECK(nvs_set_str(my_handle, "Chat_model", model));
            ESP_ERROR_CHECK(nvs_commit(my_handle));
            ESP_LOGI(TAG, "Chat_model set to %s", model);
        }
        if (nvs_erase_key(my_handle, "Claude_model") == ESP_OK) {
            ESP_ERROR_CHECK(nvs_commit(my_handle));
            ESP_LOGI(TAG, "removed obsolete key Claude_model");
        }
        // No key nor Wi-Fi is given in the firmware: they are filled in CONFIG.INI
        nvs_init_str_if_missing("Claude_key", "");
        // Gemini key (Google AI Studio), for the gemini-... models
        nvs_init_str_if_missing("Gemini_key", "");
        // OpenRouter: its key (the models offered are in AGENTS.INI)
        nvs_init_str_if_missing("OpenRouter_key", "");
        // Free OpenRouter models: read from its live list (on / off) and how many offered
        nvs_init_str_if_missing("OpenRouter_free", CONFIG_ASSISTANT_OPENROUTER_FREE);
        char free_models[8];
        snprintf(free_models, sizeof(free_models), "%d", CONFIG_ASSISTANT_FREE_MODELS);
        nvs_init_str_if_missing("Free_models", free_models);
        // Reading speed of the answer on screen, in characters per second
        char scroll_speed[8];
        snprintf(scroll_speed, sizeof(scroll_speed), "%d", CONFIG_ASSISTANT_REPLY_SCROLL_CPS);
        nvs_init_str_if_missing("Scroll_speed", scroll_speed);
        // Screen off after this many seconds without presence (radar of the BOX-3-SENSOR dock), 0: never
        char number[8];
        snprintf(number, sizeof(number), "%d", CONFIG_ASSISTANT_SCREEN_OFF_S);
        nvs_init_str_if_missing("Screen_off_s", number);
        // Time and date on the sleep screen (on / off)
        nvs_init_str_if_missing("Show_clock", CONFIG_ASSISTANT_SHOW_CLOCK);
        // Silence after which the question is considered finished (same default as ASSISTANT_END_OF_SPEECH_MS)
        snprintf(number, sizeof(number), "%d", CONFIG_ASSISTANT_END_OF_SPEECH_MS);
        nvs_init_str_if_missing("End_speech_ms", number);
        // How the answer scrolls: "line" (one line at a time) or "smooth" (continuous)
        nvs_init_str_if_missing("Scroll_mode", CONFIG_ASSISTANT_SCROLL_MODE);
        // Speaker volume (also changed by voice)
        snprintf(number, sizeof(number), "%d", CONFIG_VOLUME_LEVEL);
        nvs_init_str_if_missing("Volume", number);
        // Voice of the answers (same default as the assistant's ASSISTANT_TTS_VOICE)
        nvs_init_str_if_missing("Voice", CONFIG_ASSISTANT_TTS_VOICE);
        // Who the assistant is and who it talks to, added to its instructions: edited in PERSONA.TXT
        // (components/esp_tinyuf2, a copy of the tinyuf2 component extended for it), not in CONFIG.INI
        nvs_init_str_if_missing("Persona", "");
        // Notes the assistant was asked to remember ("souviens-toi que ..."): edited in MEMORY.TXT
        nvs_init_str_if_missing("Memory", "");
        // Where the user lives: 2-letter country code (BE, FR...) and town, for the web search and the model
        nvs_init_str_if_missing("Country", "");
        nvs_init_str_if_missing("City", "");
        // Home automation: MCP server of a Homey Pro (e.g. http://192.168.0.10:52199/mcp) and its API key
        nvs_init_str_if_missing("Homey_url", "");
        nvs_init_str_if_missing("Homey_key", "");
        // Homey device classes confirmed before being turned off ("socket": power plugs), "off" for none
        nvs_init_str_if_missing("Confirm_off", CONFIG_ASSISTANT_CONFIRM_OFF);
    }
    nvs_close(my_handle);

    // The phrase files of the voice commands are on SPIFFS (shown as VOICE_<LANG>.TXT)
    if (bsp_spiffs_mount() != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS not mounted: the phrase files are not shown");
    }

    /* install UF2 NVS */
    tinyuf2_nvs_config_t nvs_config = DEFAULT_TINYUF2_NVS_CONFIG();
    nvs_config.part_name = uf2_nvs_partition;
    nvs_config.namespace_name = uf2_nvs_namespace;
    nvs_config.modified_cb = uf2_nvs_modified_cb;

    ESP_ERROR_CHECK(esp_tinyuf2_install(NULL, &nvs_config));

    bsp_i2c_init();

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = 0,
        .flags = {
            .buff_dma = true,
        }
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();
    ui_init();
    ui_localize(lang);

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(s_event_group, NVS_MODIFIED_BIT,
                                               pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & NVS_MODIFIED_BIT) {
            esp_err_t err = nvs_open_from_partition(uf2_nvs_partition, uf2_nvs_namespace, NVS_READONLY, &my_handle);

            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to open NVS partition: %s", esp_err_to_name(err));
                // Handle the error or take appropriate action
                return;
            }

            size_t buf_len_long = sizeof(ssid);
            err = nvs_get_str(my_handle, "ssid", ssid, &buf_len_long);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to read 'ssid' from NVS: %s", esp_err_to_name(err));
                nvs_close(my_handle);
                return;
            }
            ESP_LOGD(TAG, "SSID: %s", ssid);

            buf_len_long = sizeof(password);
            err = nvs_get_str(my_handle, "password", password, &buf_len_long);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to read 'password' from NVS: %s", esp_err_to_name(err));
                nvs_close(my_handle);
                return;
            }
            ESP_LOGD(TAG, "Password updated");

            buf_len_long = sizeof(key);
            err = nvs_get_str(my_handle, "ChatGPT_key", key, &buf_len_long);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to read 'ChatGPT_key' from NVS: %s", esp_err_to_name(err));
                nvs_close(my_handle);
                return;
            }
            ESP_LOGD(TAG, "OpenAI Key updated");

            buf_len_long = sizeof(url);
            err = nvs_get_str(my_handle, "Base_url", url, &buf_len_long);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to read 'BASE_url' from NVS: %s", esp_err_to_name(err));
                nvs_close(my_handle);
                return;
            }
            ESP_LOGD(TAG, "BASE url: %s", url);
            nvs_close(my_handle);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

}
