/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_check.h"
#include "bsp/esp-bsp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "settings.h"
#include "bsp_board.h"
#include "esp_ota_ops.h"

static const char *TAG = "settings";
const char *uf2_nvs_partition = "nvs";
const char *uf2_nvs_namespace = "configuration";
static nvs_handle_t my_handle;
/* The parameters: PSRAM (internal RAM is scarce), allocated zeroed at the first use */
static sys_param_t *s_sys_param = NULL;

static sys_param_t *sys_param(void)
{
    if (!s_sys_param) {
        s_sys_param = heap_caps_calloc(1, sizeof(sys_param_t), MALLOC_CAP_SPIRAM);
        assert(s_sys_param);
    }
    return s_sys_param;
}
#define g_sys_param (*sys_param())

esp_err_t settings_factory_reset(void)
{
    const esp_partition_t *update_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    ESP_LOGI(TAG, "Switch to partition UF2");
    esp_ota_set_boot_partition(update_partition);
    // No pop from the speaker when the audio output stops with the restart
    bsp_codec_mute_set(true);
    bsp_codec_power_amp_off();
    vTaskDelay(pdMS_TO_TICKS(30));
    esp_restart();
    return ESP_OK;
}

char *settings_text_without_comments(const char *text)
{
    text = text ? text : "";
    char *out = malloc(strlen(text) + 1);
    if (!out) {
        return NULL;
    }
    size_t n = 0;
    for (const char *line = text; *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line + 1) : strlen(line);
        const char *p = line;
        while (p < line + len && (*p == ' ' || *p == '\t')) {
            p++;
        }
        if (*p != '#') {
            memcpy(out + n, line, len);
            n += len;
        }
        line += len;
    }
    while (n > 0 && (unsigned char)out[n - 1] <= ' ') {
        n--;
    }
    out[n] = '\0';
    size_t start = 0;
    while (start < n && (unsigned char)out[start] <= ' ') {
        start++;
    }
    memmove(out, out + start, n - start + 1);
    return out;
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
    if (ret != ESP_OK || len <= 1) {
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
    if (ret != ESP_OK || len <= 1) {
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

    // OpenRouter (optional): its key (the models offered are in AGENTS.INI)
    len = sizeof(g_sys_param.openrouter_key);
    if (nvs_get_str(my_handle, "OpenRouter_key", g_sys_param.openrouter_key, &len) != ESP_OK) {
        g_sys_param.openrouter_key[0] = '\0';
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

    // Silence after which the question is considered finished (End_speech_ms, 300 to 3000 ms)
    char end_speech[8] = {0};
    len = sizeof(end_speech);
    g_sys_param.end_speech_ms = CONFIG_ASSISTANT_END_OF_SPEECH_MS;
    if (nvs_get_str(my_handle, "End_speech_ms", end_speech, &len) == ESP_OK && atoi(end_speech) > 0) {
        g_sys_param.end_speech_ms = atoi(end_speech);
    }
    if (g_sys_param.end_speech_ms < 300 || g_sys_param.end_speech_ms > 3000) {
        ESP_LOGW(TAG, "End_speech_ms %d out of range (300..3000), using %d", g_sys_param.end_speech_ms,
                 CONFIG_ASSISTANT_END_OF_SPEECH_MS);
        g_sys_param.end_speech_ms = CONFIG_ASSISTANT_END_OF_SPEECH_MS;
    }

    // Time and date on the sleep screen (Show_clock = on / off, also changed by voice)
    char clock[8] = {0};
    len = sizeof(clock);
    if (nvs_get_str(my_handle, "Show_clock", clock, &len) != ESP_OK) {
        strlcpy(clock, CONFIG_ASSISTANT_SHOW_CLOCK, sizeof(clock));
    }
    str_trim(clock);
    g_sys_param.show_clock = strcasecmp(clock, "off") != 0 && strcasecmp(clock, "no") != 0 && strcmp(clock, "0") != 0;

    // After a voice command handled locally: back to sleep (on) or listening again (off)
    char after_cmd[8] = {0};
    len = sizeof(after_cmd);
    if (nvs_get_str(my_handle, "Sleep_after_cmd", after_cmd, &len) != ESP_OK) {
        strlcpy(after_cmd, CONFIG_ASSISTANT_SLEEP_AFTER_CMD, sizeof(after_cmd));
    }
    str_trim(after_cmd);
    g_sys_param.sleep_after_cmd = strcasecmp(after_cmd, "on") == 0 || strcasecmp(after_cmd, "true") == 0 ||
                                  strcasecmp(after_cmd, "yes") == 0 || strcmp(after_cmd, "1") == 0;

    // Currency of the prices of AGENTS.INI and of the costs (no conversion)
    len = sizeof(g_sys_param.currency);
    if (nvs_get_str(my_handle, "Currency", g_sys_param.currency, &len) != ESP_OK || len <= 1) {
        strlcpy(g_sys_param.currency, CONFIG_ASSISTANT_CURRENCY, sizeof(g_sys_param.currency));
    }
    str_trim(g_sys_param.currency);

    // Free OpenRouter models: read from its live list or not, and how many offered
    char value[8] = CONFIG_ASSISTANT_OPENROUTER_FREE;
    len = sizeof(value);
    if (nvs_get_str(my_handle, "OpenRouter_free", value, &len) != ESP_OK) {
        strlcpy(value, CONFIG_ASSISTANT_OPENROUTER_FREE, sizeof(value));
    }
    str_trim(value);
    g_sys_param.openrouter_free = strcasecmp(value, "off") != 0 && strcasecmp(value, "no") != 0 &&
                                  strcmp(value, "0") != 0 && strcasecmp(value, "false") != 0;
    len = sizeof(value);
    g_sys_param.free_models = CONFIG_ASSISTANT_FREE_MODELS;
    if (nvs_get_str(my_handle, "Free_models", value, &len) == ESP_OK && atoi(value) > 0) {
        g_sys_param.free_models = atoi(value) > 15 ? 15 : atoi(value);
    }

    // Screen off after this many seconds without presence (BOX-3-SENSOR radar), 0: never
    char screen_off[8] = {0};
    len = sizeof(screen_off);
    g_sys_param.screen_off_s = CONFIG_ASSISTANT_SCREEN_OFF_S;
    if (nvs_get_str(my_handle, "Screen_off_s", screen_off, &len) == ESP_OK && screen_off[0]) {
        int s = atoi(screen_off);
        g_sys_param.screen_off_s = s < 0 ? 0 : s;
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

    // Homey device classes to confirm before turning them off ("socket, heater"); "off" for none
    len = sizeof(g_sys_param.confirm_off);
    if (nvs_get_str(my_handle, "Confirm_off", g_sys_param.confirm_off, &len) != ESP_OK) {
        strlcpy(g_sys_param.confirm_off, CONFIG_ASSISTANT_CONFIRM_OFF, sizeof(g_sys_param.confirm_off));
    }
    str_trim(g_sys_param.confirm_off);
    if (strcasecmp(g_sys_param.confirm_off, "off") == 0 || strcasecmp(g_sys_param.confirm_off, "no") == 0) {
        g_sys_param.confirm_off[0] = '\0';
    }
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
    if (g_sys_param.persona) {
        // Its comment lines stay in the file only: the assistant gets the rest
        char *persona = settings_text_without_comments(g_sys_param.persona);
        if (persona) {
            free(g_sys_param.persona);
            g_sys_param.persona = persona[0] ? persona : NULL;
            if (!persona[0]) {
                free(persona);
            }
        }
    }
    free(g_sys_param.memory);
    g_sys_param.memory = read_text(my_handle, "Memory");

    // Scrolling mode of the answer (optional): "line" or "smooth"
    char mode[12] = {0};
    len = sizeof(mode);
    if (nvs_get_str(my_handle, "Scroll_mode", mode, &len) != ESP_OK) {
        strlcpy(mode, CONFIG_ASSISTANT_SCROLL_MODE, sizeof(mode));
    }
    str_trim(mode);
    g_sys_param.scroll_smooth = (strcasecmp(mode, "smooth") == 0);

    nvs_close(my_handle);

    // SSID and password may legitimately contain spaces: only the other values are trimmed
    str_trim(g_sys_param.key);
    str_trim(g_sys_param.url);
    str_trim(g_sys_param.lang);
    str_trim(g_sys_param.claude_key);
    str_trim(g_sys_param.gemini_key);
    str_trim(g_sys_param.openrouter_key);
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
    ESP_LOGI(TAG, "chat model:%s, Claude key: (%d chars), Gemini key: (%d chars), OpenRouter key: (%d chars)",
             g_sys_param.chat_model, (int)strlen(g_sys_param.claude_key), (int)strlen(g_sys_param.gemini_key),
             (int)strlen(g_sys_param.openrouter_key));
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

/* Kept by the soft reset: Wi-Fi, API keys, Homey, PERSONA.TXT, MEMORY.TXT, comments of CONFIG.INI */
static const char *const s_kept_keys[] = {
    "ssid", "password", "ChatGPT_key", "Base_url", "Claude_key", "Gemini_key", "OpenRouter_key",
    "Homey_url", "Homey_key", "Persona", "Memory", "Config_layout",
};

static bool is_kept_key(const char *key)
{
    for (size_t i = 0; i < sizeof(s_kept_keys) / sizeof(s_kept_keys[0]); i++) {
        if (strcmp(key, s_kept_keys[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Values of the configuration mode's first start (factory_nvs, same Kconfig values); any other key's is empty */
#define STR_OF(x)   #x
#define STR(x)      STR_OF(x)
static const struct {
    const char *key;
    const char *value;
} s_default_values[] = {
    { "Base_url", CONFIG_ASSISTANT_OPENAI_URL },
    { "Language", CONFIG_ASSISTANT_LANGUAGE },
    { "Chat_model", CONFIG_ASSISTANT_CHAT_MODEL },
    { "Voice", CONFIG_ASSISTANT_TTS_VOICE },
    { "Scroll_speed", STR(CONFIG_ASSISTANT_REPLY_SCROLL_CPS) },
    { "Scroll_mode", CONFIG_ASSISTANT_SCROLL_MODE },
    { "OpenRouter_free", CONFIG_ASSISTANT_OPENROUTER_FREE },
    { "Free_models", STR(CONFIG_ASSISTANT_FREE_MODELS) },
    { "Confirm_off", CONFIG_ASSISTANT_CONFIRM_OFF },
    { "Show_clock", CONFIG_ASSISTANT_SHOW_CLOCK },
    { "Sleep_after_cmd", CONFIG_ASSISTANT_SLEEP_AFTER_CMD },
    { "Currency", CONFIG_ASSISTANT_CURRENCY },
    { "Volume", STR(CONFIG_VOLUME_LEVEL) },
    { "Screen_off_s", STR(CONFIG_ASSISTANT_SCREEN_OFF_S) },
    { "End_speech_ms", STR(CONFIG_ASSISTANT_END_OF_SPEECH_MS) },
};

static bool is_default_value(const char *key, const char *value)
{
    if (!value[0] || strcmp(key, "Config_layout") == 0) {
        return true;
    }
    for (size_t i = 0; i < sizeof(s_default_values) / sizeof(s_default_values[0]); i++) {
        if (strcmp(key, s_default_values[i].key) == 0) {
            return strcmp(value, s_default_values[i].value) == 0;
        }
    }
    return false;
}

/* A string of the configuration, allocated (NULL if missing) */
static char *get_str(nvs_handle_t handle, const char *key)
{
    size_t len = 0;
    if (nvs_get_str(handle, key, NULL, &len) != ESP_OK) {
        return NULL;
    }
    char *value = malloc(len);
    if (value && nvs_get_str(handle, key, value, &len) != ESP_OK) {
        free(value);
        value = NULL;
    }
    return value;
}

/* CONFIG.INI differs from the firmware's: a value was changed (from the PC, by voice or from the screen) */
static bool config_modified(nvs_handle_t handle)
{
    bool modified = false;
    nvs_iterator_t it = NULL;
    esp_err_t res = nvs_entry_find(uf2_nvs_partition, uf2_nvs_namespace, NVS_TYPE_STR, &it);
    while (res == ESP_OK && !modified) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        char *value = get_str(handle, info.key);
        modified = value && !is_default_value(info.key, value);
        if (modified) {
            ESP_LOGI(TAG, "%s differs from the firmware's", info.key);
        }
        free(value);
        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return modified;
}

/* A multi-line text (PERSONA.TXT, MEMORY.TXT) as comment lines of CONFIG.BKP */
static void backup_text(FILE *f, const char *title, const char *text)
{
    fprintf(f, "\r\n# ---------- %s ----------\r\n", title);
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len && line[len - 1] == '\r') {
            len--;
        }
        // Its own comment lines already start with '#'
        fprintf(f, "%s%.*s\r\n", line[0] == '#' ? "" : "# ", (int)len, line);
        line = end ? end + 1 : line + len;
    }
}

/* CONFIG.INI as "key = value" lines, then PERSONA.TXT and MEMORY.TXT as comments, to SETTINGS_BACKUP_FILE */
static esp_err_t backup_config(nvs_handle_t handle)
{
    FILE *f = fopen(SETTINGS_BACKUP_FILE, "w");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "cannot write %s", SETTINGS_BACKUP_FILE);
    fprintf(f, "# CONFIG.INI saved before the factory reset (copy back the lines you want to keep)\r\n"
            "[%s]\r\n", uf2_nvs_namespace);
    nvs_iterator_t it = NULL;
    esp_err_t res = nvs_entry_find(uf2_nvs_partition, uf2_nvs_namespace, NVS_TYPE_STR, &it);
    while (res == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strcmp(info.key, "Persona") && strcmp(info.key, "Memory") && strcmp(info.key, "Config_layout")) {
            char *value = get_str(handle, info.key);
            if (value) {
                fprintf(f, "%s = %s\r\n", info.key, value);
            }
            free(value);
        }
        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    char *persona = get_str(handle, "Persona");
    char *memory = get_str(handle, "Memory");
    if (persona && persona[0]) {
        backup_text(f, "PERSONA.TXT", persona);
    }
    backup_text(f, "MEMORY.TXT", memory);
    free(persona);
    free(memory);
    // Who the assistant is, per language (PERSO_FR.TXT...)
    static const char *const s_persona_files[][2] = {
        { "/spiffs/persona_fr.txt", "PERSO_FR.TXT" }, { "/spiffs/persona_en.txt", "PERSO_EN.TXT" },
    };
    for (size_t i = 0; i < sizeof(s_persona_files) / sizeof(s_persona_files[0]); i++) {
        FILE *pf = fopen(s_persona_files[i][0], "r");
        char *text = pf ? calloc(1, 8192) : NULL;
        if (text) {
            fread(text, 1, 8191, pf);
            backup_text(f, s_persona_files[i][1], text);
        }
        if (pf) {
            fclose(pf);
        }
        free(text);
    }
    bool ok = (fclose(f) == 0);
    ESP_RETURN_ON_FALSE(ok, ESP_FAIL, TAG, "cannot write %s", SETTINGS_BACKUP_FILE);
    ESP_LOGI(TAG, "CONFIG.INI saved to %s", SETTINGS_BACKUP_FILE);
    return ESP_OK;
}

esp_err_t settings_factory_wipe(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open_from_partition(uf2_nvs_partition, uf2_nvs_namespace, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs open failed (0x%x)", ret);
    // Unchanged since the last factory reset: its backup (the settings before it) is kept
    if (config_modified(handle) && backup_config(handle) != ESP_OK) {
        nvs_close(handle);
        return ESP_FAIL;    // nothing is erased without its backup
    }
    ret = nvs_erase_all(handle);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_LOGI(TAG, "Every setting erased (%s)", esp_err_to_name(ret));
    return ret;
}

esp_err_t settings_restore_defaults(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open_from_partition(uf2_nvs_partition, uf2_nvs_namespace, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs open failed (0x%x)", ret);
    // The keys to erase first, then erased (not while iterating); the defaults apply when they are missing
    char keys[48][NVS_KEY_NAME_MAX_SIZE];
    int count = 0;
    nvs_iterator_t it = NULL;
    esp_err_t res = nvs_entry_find(uf2_nvs_partition, uf2_nvs_namespace, NVS_TYPE_ANY, &it);
    while (res == ESP_OK && count < (int)(sizeof(keys) / sizeof(keys[0]))) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (!is_kept_key(info.key)) {
            strlcpy(keys[count++], info.key, sizeof(keys[0]));
        }
        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    for (int i = 0; i < count; i++) {
        nvs_erase_key(handle, keys[i]);
        ESP_LOGI(TAG, "%s back to its default", keys[i]);
    }
    ret = nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

esp_err_t settings_erase_key(const char *key)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open_from_partition(uf2_nvs_partition, uf2_nvs_namespace, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs open failed (0x%x)", ret);
    ret = nvs_erase_key(handle, key);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_LOGI(TAG, "erased %s (%s)", key, esp_err_to_name(ret));
    return ret;
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
