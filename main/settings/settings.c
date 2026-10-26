/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_check.h"
#include "bsp/esp-bsp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "settings.h"
#include "esp_ota_ops.h"

static const char *TAG = "settings";
const char *uf2_nvs_partition = "nvs";
const char *uf2_nvs_namespace = "configuration";
static nvs_handle_t my_handle;
static sys_param_t g_sys_param = {0};

esp_err_t settings_factory_reset(void)
{
    const esp_partition_t *update_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    ESP_LOGI(TAG, "Switch to partition UF2");
    esp_ota_set_boot_partition(update_partition);
    esp_restart();
    return ESP_OK;
}

/* Spaces, control characters and non-ASCII bytes (e.g. a pasted non-breaking space) */
static bool is_trimmed_char(char c)
{
    return (unsigned char)c <= ' ' || (unsigned char)c >= 0x7f;
}

/* Clean up an ASCII value (key, URL, model...) pasted in CONFIG.INI */
static void str_trim(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && is_trimmed_char(s[len - 1])) {
        s[--len] = '\0';
    }
    size_t start = 0;
    while (start < len && is_trimmed_char(s[start])) {
        start++;
    }
    if (start > 0) {
        memmove(s, s + start, len - start + 1);
    }
}

/* A free text (may contain accents, so only spaces and line endings are trimmed), NULL if absent or empty */
static char *read_text(nvs_handle_t handle, const char *key)
{
    size_t len = 0;
    if (nvs_get_str(handle, key, NULL, &len) != ESP_OK || len <= 1) {
        return NULL;
    }
    char *p = malloc(len);
    if (!p || nvs_get_str(handle, key, p, &len) != ESP_OK) {
        free(p);
        return NULL;
    }
    size_t n = strlen(p);
    while (n > 0 && (unsigned char)p[n - 1] <= ' ') {
        p[--n] = '\0';
    }
    size_t start = 0;
    while (start < n && (unsigned char)p[start] <= ' ') {
        start++;
    }
    memmove(p, p + start, n - start + 1);
    if (p[0] == '\0') {
        free(p);
        return NULL;
    }
    return p;
}

esp_err_t settings_read_parameter_from_nvs(void)
{
    esp_err_t ret = nvs_open_from_partition(uf2_nvs_partition, uf2_nvs_namespace, NVS_READONLY, &my_handle);
    if (ESP_ERR_NVS_NOT_FOUND == ret) {
        ESP_LOGI(TAG, "Credentials not found");
        goto err;
    }

    ESP_GOTO_ON_FALSE(ESP_OK == ret, ret, err, TAG, "nvs open failed (0x%x)", ret);
    size_t len = 0;

    // Read SSID
    len = sizeof(g_sys_param.ssid);
    ret = nvs_get_str(my_handle, "ssid", g_sys_param.ssid, &len);
    if (ret != ESP_OK || len == 0) {
        ESP_LOGI(TAG, "No SSID found");
        goto err;
    }

    // Read password
    len = sizeof(g_sys_param.password);
    ret = nvs_get_str(my_handle, "password", g_sys_param.password, &len);
    if (ret != ESP_OK || len == 0) {
        ESP_LOGI(TAG, "No Password found");
        goto err;
    }

    // Read key
    len = sizeof(g_sys_param.key);
    ret = nvs_get_str(my_handle, "ChatGPT_key", g_sys_param.key, &len);
    if (ret == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGE(TAG, "OpenAI key longer than %u characters", (unsigned)sizeof(g_sys_param.key) - 1);
    }
    if (ret != ESP_OK || len == 0) {
        ESP_LOGI(TAG, "No OpenAI key found");
        goto err;
    }

    // Read url
    len = sizeof(g_sys_param.url);
    ret = nvs_get_str(my_handle, "Base_url", g_sys_param.url, &len);
    if (ret != ESP_OK || len == 0) {
        ESP_LOGI(TAG, "No OpenAI Base url found");
        goto err;
    }

    // Read language (optional, written by factory_nvs since the language support was added)
    len = sizeof(g_sys_param.lang);
    ret = nvs_get_str(my_handle, "Language", g_sys_param.lang, &len);
    if (ret != ESP_OK || len <= 1) {
        strlcpy(g_sys_param.lang, CONFIG_ASSISTANT_LANGUAGE, sizeof(g_sys_param.lang));
    }


    // Read the chat model (optional; "Claude_model" before the GPT models were added)
    len = sizeof(g_sys_param.chat_model);
    ret = nvs_get_str(my_handle, "Chat_model", g_sys_param.chat_model, &len);
    if (ret != ESP_OK || len <= 1) {
        len = sizeof(g_sys_param.chat_model);
        ret = nvs_get_str(my_handle, "Claude_model", g_sys_param.chat_model, &len);
    }
    if (ret != ESP_OK || len <= 1) {
        strlcpy(g_sys_param.chat_model, CONFIG_ASSISTANT_CHAT_MODEL, sizeof(g_sys_param.chat_model));
    }

    // Read the Claude key (optional)
    len = sizeof(g_sys_param.claude_key);
    ret = nvs_get_str(my_handle, "Claude_key", g_sys_param.claude_key, &len);
    if (ret == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGE(TAG, "Claude key longer than %u characters", (unsigned)sizeof(g_sys_param.claude_key) - 1);
    }
    if (ret != ESP_OK) {
        g_sys_param.claude_key[0] = '\0';
    }

    // Read the Gemini key (optional)
    len = sizeof(g_sys_param.gemini_key);
    if (nvs_get_str(my_handle, "Gemini_key", g_sys_param.gemini_key, &len) != ESP_OK) {
        g_sys_param.gemini_key[0] = '\0';
    }

    // Read the scrolling speed of the answer (optional)
    char speed[8] = {0};
    len = sizeof(speed);
    g_sys_param.scroll_cps = CONFIG_ASSISTANT_REPLY_SCROLL_CPS;
    if (nvs_get_str(my_handle, "Scroll_speed", speed, &len) == ESP_OK && atoi(speed) > 0) {
        g_sys_param.scroll_cps = atoi(speed);
    }
    if (g_sys_param.scroll_cps < 5 || g_sys_param.scroll_cps > 60) {
        ESP_LOGW(TAG, "Scroll_speed %d out of range (5..60), using %d", g_sys_param.scroll_cps,
                 CONFIG_ASSISTANT_REPLY_SCROLL_CPS);
        g_sys_param.scroll_cps = CONFIG_ASSISTANT_REPLY_SCROLL_CPS;
    }

    // Speaker volume (optional, saved when changed by voice)
    char volume[8] = {0};
    len = sizeof(volume);
    g_sys_param.volume = CONFIG_VOLUME_LEVEL;
    if (nvs_get_str(my_handle, "Volume", volume, &len) == ESP_OK && volume[0]) {
        int level = atoi(volume);
        g_sys_param.volume = level < 0 ? 0 : (level > 100 ? 100 : level);
    }

    // Where the user lives (optional): localizes the web search and tells the model
    len = sizeof(g_sys_param.country);
    if (nvs_get_str(my_handle, "Country", g_sys_param.country, &len) != ESP_OK) {
        g_sys_param.country[0] = '\0';
    }
    str_trim(g_sys_param.country);
    for (char *p = g_sys_param.country; *p; p++) {
        *p = toupper((unsigned char)*p);
    }
    if (strlen(g_sys_param.country) != 2 || !isalpha((unsigned char)g_sys_param.country[0]) ||
            !isalpha((unsigned char)g_sys_param.country[1])) {
        if (g_sys_param.country[0]) {
            ESP_LOGW(TAG, "Country \"%s\" ignored: a 2-letter code is expected (BE, FR...)", g_sys_param.country);
        }
        g_sys_param.country[0] = '\0';
    }
    len = sizeof(g_sys_param.city);
    if (nvs_get_str(my_handle, "City", g_sys_param.city, &len) != ESP_OK) {
        g_sys_param.city[0] = '\0';
    }

    // Home automation (optional): MCP server of a Homey Pro and its API key
    len = sizeof(g_sys_param.homey_url);
    if (nvs_get_str(my_handle, "Homey_url", g_sys_param.homey_url, &len) != ESP_OK) {
        g_sys_param.homey_url[0] = '\0';
    }
    len = sizeof(g_sys_param.homey_key);
    if (nvs_get_str(my_handle, "Homey_key", g_sys_param.homey_key, &len) != ESP_OK) {
        g_sys_param.homey_key[0] = '\0';
    }
    str_trim(g_sys_param.homey_url);
    str_trim(g_sys_param.homey_key);
    ESP_LOGI(TAG, "Homey: %s, key: (%d chars)", g_sys_param.homey_url[0] ? g_sys_param.homey_url : "-",
             (int)strlen(g_sys_param.homey_key));

    // Voice of the answers (optional)
    len = sizeof(g_sys_param.voice);
    if (nvs_get_str(my_handle, "Voice", g_sys_param.voice, &len) != ESP_OK || len <= 1) {
        strlcpy(g_sys_param.voice, CONFIG_ASSISTANT_TTS_VOICE, sizeof(g_sys_param.voice));
    }
    str_trim(g_sys_param.voice);
    for (char *p = g_sys_param.voice; *p; p++) {
        *p = tolower((unsigned char)*p);
    }

    // Persona and Memory (optional): free texts, edited in PERSONA.TXT and MEMORY.TXT
    free(g_sys_param.persona);
    g_sys_param.persona = read_text(my_handle, "Persona");
    free(g_sys_param.memory);
    g_sys_param.memory = read_text(my_handle, "Memory");

    // Scrolling mode of the answer (optional): "line" (default) or "smooth"
    char mode[12] = {0};
    len = sizeof(mode);
    g_sys_param.scroll_smooth = false;
    if (nvs_get_str(my_handle, "Scroll_mode", mode, &len) == ESP_OK) {
        str_trim(mode);
        g_sys_param.scroll_smooth = (strcasecmp(mode, "smooth") == 0);
    }

    nvs_close(my_handle);

    // SSID and password may legitimately contain spaces: only the other values are trimmed
    str_trim(g_sys_param.key);
    str_trim(g_sys_param.url);
    str_trim(g_sys_param.lang);
    str_trim(g_sys_param.claude_key);
    str_trim(g_sys_param.gemini_key);
    str_trim(g_sys_param.chat_model);

    ESP_LOGI(TAG, "stored ssid:%s", g_sys_param.ssid);
    ESP_LOGI(TAG, "stored password: (%d chars)", (int)strlen(g_sys_param.password));
    ESP_LOGI(TAG, "stored OpenAI key: (%d chars)", (int)strlen(g_sys_param.key));
    ESP_LOGI(TAG, "stored Base URL:%s", g_sys_param.url);
    ESP_LOGI(TAG, "volume:%d, country:%s, city:%s", g_sys_param.volume, g_sys_param.country, g_sys_param.city);
    ESP_LOGI(TAG, "voice:%s, persona: %d chars, memory: %d chars", g_sys_param.voice,
             g_sys_param.persona ? (int)strlen(g_sys_param.persona) : 0,
             g_sys_param.memory ? (int)strlen(g_sys_param.memory) : 0);
    ESP_LOGI(TAG, "language:%s, answer scrolling: %d characters/s, %s", g_sys_param.lang, g_sys_param.scroll_cps,
             g_sys_param.scroll_smooth ? "smooth" : "line by line");
    ESP_LOGI(TAG, "chat model:%s, Claude key: (%d chars), Gemini key: (%d chars)", g_sys_param.chat_model,
             (int)strlen(g_sys_param.claude_key), (int)strlen(g_sys_param.gemini_key));
    return ESP_OK;

err:
    if (my_handle) {
        nvs_close(my_handle);
    }
    settings_factory_reset();
    return ret;
}

sys_param_t *settings_get_parameter(void)
{
    return &g_sys_param;
}

esp_err_t settings_save_str(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open_from_partition(uf2_nvs_partition, uf2_nvs_namespace, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs open failed (0x%x)", ret);
    ret = nvs_set_str(handle, key, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to save %s (0x%x)", key, ret);
    ESP_LOGI(TAG, "saved %s:%s", key, value);
    return ESP_OK;
}
