/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <ctype.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "debug_trace.h"
#include "file_costs.h"
#include "file_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_check.h"
#include "bsp/esp-bsp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "settings.h"
#include "google_auth.h"
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
    // The costs as read on the configuration drive (COSTS.TXT: not written while the assistant works), the starts
    costs_write_text();
    state_boots_export();
    // The page linking the Google account (GOOGLE.HTM), while it is not linked
    google_auth_export_link();
    // The traces, if on, to be read on the configuration drive (DEBUG.TXT)
    if (debug_trace_on()) {
        debug_trace_dump();
    }
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

/* A string setting into buf (an ASCII value trimmed, see str_trim), "" when absent: false if absent or empty */
static bool read_str(const char *key, char *buf, size_t size, bool trim)
{
    size_t len = size;
    esp_err_t ret = nvs_get_str(my_handle, key, buf, &len);
    if (ret == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGE(TAG, "%s longer than %u characters", key, (unsigned)size - 1);
    }
    if (ret != ESP_OK) {
        buf[0] = '\0';
        return false;
    }
    if (trim) {
        str_trim(buf);
    }
    return buf[0] != '\0';
}

/* The same, def (of Kconfig) when absent or empty */
static void read_str_or(const char *key, char *buf, size_t size, const char *def, bool trim)
{
    if (!read_str(key, buf, size, trim)) {
        strlcpy(buf, def, size);
    }
}

/* What is done with a number out of its range */
typedef enum {
    OUT_OF_RANGE_DEFAULT,       /* the default used, with a warning */
    OUT_OF_RANGE_CLAMP,         /* brought to the nearest bound */
} out_of_range_t;

/* A whole number in min..max, def when absent or not a number */
static int read_int(const char *key, int def, int min, int max, out_of_range_t out)
{
    char value[12];
    if (!read_str(key, value, sizeof(value), true) || !(isdigit((unsigned char)value[0]) ||
            (value[0] == '-' && isdigit((unsigned char)value[1])))) {
        return def;
    }
    int n = atoi(value);
    if (n >= min && n <= max) {
        return n;
    }
    if (out == OUT_OF_RANGE_CLAMP) {
        return n < min ? min : max;
    }
    ESP_LOGW(TAG, "%s %d out of range (%d..%d), using %d", key, n, min, max, def);
    return def;
}

/* "on" / "off" (also yes / no, true / false, 1 / 0), def (of Kconfig) when absent or neither */
static bool is_on(const char *value, bool *on)
{
    static const char *const yes[] = { "on", "yes", "true", "1" };
    static const char *const no[] = { "off", "no", "false", "0" };
    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
        if (strcasecmp(value, yes[i]) == 0 || strcasecmp(value, no[i]) == 0) {
            *on = strcasecmp(value, yes[i]) == 0;
            return true;
        }
    }
    return false;
}

static bool read_bool(const char *key, const char *def)
{
    char value[8];
    bool on = false;
    if (read_str(key, value, sizeof(value), true) && is_on(value, &on)) {
        return on;
    }
    return is_on(def, &on) && on;
}

/* The keys of the providers, all optional but ChatGPT_key (read first): trimmed (a pasted key may carry spaces) */
#define FIELD(f)    offsetof(sys_param_t, f), sizeof(((sys_param_t *)0)->f)
static const struct {
    const char *key;
    size_t offset;
    size_t size;
} s_api_keys[] = {
    { "Claude_key", FIELD(claude_key) },
    { "Gemini_key", FIELD(gemini_key) },
    { "OpenRouter_key", FIELD(openrouter_key) },
    { "XAI_key", FIELD(xai_key) },
    { "Mistral_key", FIELD(mistral_key) },
    { "Perplexity_key", FIELD(perplexity_key) },
    { "Deepgram_key", FIELD(deepgram_key) },
    { "Groq_key", FIELD(groq_key) },
    { "ElevenLabs_key", FIELD(elevenlabs_key) },
};

/*
 * Homey_url / Homey_key of older versions (and Homey_tools / Homey_on of 2.31.0): an MCP server like the others,
 * McpN_url, McpN_key, McpN_tools, McpN_on with McpN_name = Homey, under the first free number; the old keys erased
 * (the empty ones the configuration mode made too)
 */
static void homey_migrate(void)
{
    static const char *const old_keys[] = { "Homey_url", "Homey_key", "Homey_tools", "Homey_on" };
    static const char *const fields[] = { "url", "key", "tools", "on" };
    nvs_handle_t handle;
    if (nvs_open_from_partition(uf2_nvs_partition, uf2_nvs_namespace, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    char *url = read_text(handle, "Homey_url");
    if (url) {
        int free_n = 0;
        bool already = false;
        for (int n = 1; n <= 99; n++) {
            char key[16];
            snprintf(key, sizeof(key), "Mcp%d_url", n);
            char *other = read_text(handle, key);
            if (!other && !free_n) {
                free_n = n;
            }
            already |= other && strcmp(other, url) == 0;
            free(other);
        }
        if (!already && free_n) {
            char key[16];
            snprintf(key, sizeof(key), "Mcp%d_name", free_n);
            nvs_set_str(handle, key, "Homey");
            for (size_t i = 0; i < sizeof(old_keys) / sizeof(old_keys[0]); i++) {
                char *value = read_text(handle, old_keys[i]);
                if (value) {
                    snprintf(key, sizeof(key), "Mcp%d_%s", free_n, fields[i]);
                    nvs_set_str(handle, key, value);
                    free(value);
                }
            }
            ESP_LOGI(TAG, "Homey moved to the MCP servers: Mcp%d", free_n);
        } else {
            ESP_LOGW(TAG, "Homey_url %s: dropped", already ? "already an MCP server" : "not moved (no number free)");
        }
        free(url);
    }
    bool erased = false;
    for (size_t i = 0; i < sizeof(old_keys) / sizeof(old_keys[0]); i++) {
        erased |= (nvs_erase_key(handle, old_keys[i]) == ESP_OK);
    }
    if (url || erased) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

esp_err_t settings_read_parameter_from_nvs(void)
{
    homey_migrate();
    esp_err_t ret = nvs_open_from_partition(uf2_nvs_partition, uf2_nvs_namespace, NVS_READONLY, &my_handle);
    if (ESP_ERR_NVS_NOT_FOUND == ret) {
        ESP_LOGI(TAG, "Credentials not found");
        goto err;
    }
    ESP_GOTO_ON_FALSE(ESP_OK == ret, ret, err, TAG, "nvs open failed (0x%x)", ret);

    // Wi-Fi (the SSID and the password may legitimately contain spaces: not trimmed) and the OpenAI key: required
    if (!read_str("ssid", g_sys_param.ssid, sizeof(g_sys_param.ssid), false)) {
        ESP_LOGI(TAG, "No SSID found");
        ret = ESP_ERR_NOT_FOUND;
        goto err;
    }
    size_t len = sizeof(g_sys_param.password);
    if (nvs_get_str(my_handle, "password", g_sys_param.password, &len) != ESP_OK) {
        ESP_LOGI(TAG, "No Password found");
        ret = ESP_ERR_NOT_FOUND;
        goto err;
    }
    if (!read_str("ChatGPT_key", g_sys_param.key, sizeof(g_sys_param.key), true)) {
        ESP_LOGI(TAG, "No OpenAI key found");
        ret = ESP_ERR_NOT_FOUND;
        goto err;
    }
    for (size_t i = 0; i < sizeof(s_api_keys) / sizeof(s_api_keys[0]); i++) {
        read_str(s_api_keys[i].key, (char *)&g_sys_param + s_api_keys[i].offset, s_api_keys[i].size, true);
    }

    // OpenAI's API: fixed (no longer in CONFIG.INI; a Base_url key of an older version is erased by the
    // configuration mode)
    strlcpy(g_sys_param.url, CONFIG_ASSISTANT_OPENAI_URL, sizeof(g_sys_param.url));

    read_str_or("Language", g_sys_param.lang, sizeof(g_sys_param.lang), CONFIG_ASSISTANT_LANGUAGE, true);
    // The chat model ("Claude_model" before the GPT models were added)
    if (!read_str("Chat_model", g_sys_param.chat_model, sizeof(g_sys_param.chat_model), true)) {
        read_str_or("Claude_model", g_sys_param.chat_model, sizeof(g_sys_param.chat_model),
                    CONFIG_ASSISTANT_CHAT_MODEL, true);
    }
    // Languages offered (Settings screen, "change la langue"): "FR:Français, EN:English" (accents: not trimmed)
    read_str_or("Languages", g_sys_param.languages, sizeof(g_sys_param.languages), CONFIG_ASSISTANT_LANGUAGES, false);
    // Currency of the prices of AGENTS.INI and of the costs (no conversion)
    read_str_or("Currency", g_sys_param.currency, sizeof(g_sys_param.currency), CONFIG_ASSISTANT_CURRENCY, true);
    // Voice of the answers, transcription and speech models ([stt] and [tts] of AGENTS.INI)
    read_str_or("Voice", g_sys_param.voice, sizeof(g_sys_param.voice), CONFIG_ASSISTANT_TTS_VOICE, true);
    read_str_or("Stt_model", g_sys_param.stt_model, sizeof(g_sys_param.stt_model), CONFIG_ASSISTANT_STT_MODEL, true);
    read_str_or("Tts_model", g_sys_param.tts_model, sizeof(g_sys_param.tts_model), CONFIG_ASSISTANT_TTS_MODEL, true);

    // Reading speed of the answer on screen; silence after which the question is considered finished
    g_sys_param.scroll_cps = read_int("Scroll_speed", CONFIG_ASSISTANT_REPLY_SCROLL_CPS, 5, 60, OUT_OF_RANGE_DEFAULT);
    g_sys_param.end_speech_ms = read_int("End_speech_ms", CONFIG_ASSISTANT_END_OF_SPEECH_MS, 300, 3000,
                                         OUT_OF_RANGE_DEFAULT);
    // Scrolling mode of the answer: "line" or "smooth"
    char mode[12];
    read_str_or("Scroll_mode", mode, sizeof(mode), CONFIG_ASSISTANT_SCROLL_MODE, true);
    g_sys_param.scroll_smooth = (strcasecmp(mode, "smooth") == 0);
    // Time and date on the sleep screen (also changed by voice)
    g_sys_param.show_clock = read_bool("Show_clock", CONFIG_ASSISTANT_SHOW_CLOCK);
    // After a voice command handled locally: back to sleep (on) or listening again (off)
    g_sys_param.sleep_after_cmd = read_bool("Sleep_after_cmd", CONFIG_ASSISTANT_SLEEP_AFTER_CMD);
    // Lists of the voice commands read aloud
    g_sys_param.read_lists = read_bool("Read_lists", CONFIG_ASSISTANT_READ_LISTS);
    // The beep when listening again after a reply (Paramètres page of the Settings screen)
    g_sys_param.followup_beep = read_bool("Followup_beep", CONFIG_ASSISTANT_FOLLOWUP_BEEP);
    // Debug traces (also switched on the Debug page of the Settings screen)
    g_sys_param.debug = read_bool("Debug", CONFIG_ASSISTANT_DEBUG);

    // Free OpenRouter models offered (Free_models of CONFIG.INI, 0 to 5, the most used; 0: none, the list is not
    // read), kept as OpenRouter_free. An older version's "on" / "off" there (until the configuration mode merges
    // it): "off" is 0, "on" its Free_models
    g_sys_param.free_models = CONFIG_ASSISTANT_FREE_MODELS;
    char old_free[8];
    bool on = true;
    if (read_str("OpenRouter_free", old_free, sizeof(old_free), true)) {
        if (isdigit((unsigned char)old_free[0])) {
            g_sys_param.free_models = read_int("OpenRouter_free", CONFIG_ASSISTANT_FREE_MODELS, 0, 5,
                                               OUT_OF_RANGE_CLAMP);
        } else if (is_on(old_free, &on) && !on) {
            g_sys_param.free_models = 0;
        } else {
            g_sys_param.free_models = read_int("Free_models", CONFIG_ASSISTANT_FREE_MODELS, 0, 5, OUT_OF_RANGE_CLAMP);
        }
    }

    // Reasoning mode: asked whether to go on with the conversation after this pause, 0: never
    g_sys_param.resume_ask_min = read_int("Resume_ask_min", CONFIG_ASSISTANT_RESUME_ASK_MIN, 0, 120,
                                          OUT_OF_RANGE_CLAMP);
    // A timer or an alarm not stopped stops ringing by itself after this (minutes)
    g_sys_param.alarm_stop_min = read_int("Alarm_stop_min", CONFIG_ASSISTANT_ALARM_STOP_MIN, 1, 60,
                                          OUT_OF_RANGE_CLAMP);
    // Screen off after this many seconds without presence (BOX-3-SENSOR radar), 0: never
    g_sys_param.screen_off_s = read_int("Screen_off_s", CONFIG_ASSISTANT_SCREEN_OFF_S, 0, INT_MAX, OUT_OF_RANGE_CLAMP);
    // Radio: treble in dB, level the stations are brought to in dBFS, resumed after a conversation
    g_sys_param.radio_treble = read_int("Radio_treble", CONFIG_ASSISTANT_RADIO_TREBLE, -12, 6, OUT_OF_RANGE_CLAMP);
    g_sys_param.radio_level = read_int("Radio_level", CONFIG_ASSISTANT_RADIO_LEVEL, -30, -6, OUT_OF_RANGE_CLAMP);
    g_sys_param.radio_resume = read_bool("Radio_resume", CONFIG_ASSISTANT_RADIO_RESUME);
    // Speaker volume (saved when changed by voice)
    g_sys_param.volume = read_int("Volume", CONFIG_VOLUME_LEVEL, 0, 100, OUT_OF_RANGE_CLAMP);

    // Where the user lives: localizes the web search and tells the model (a town may end with an accent)
    read_str("Country", g_sys_param.country, sizeof(g_sys_param.country), true);
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
    read_str("City", g_sys_param.city, sizeof(g_sys_param.city), false);

    // Home automation (a Homey among the MCP servers, mcp_client.c): the device classes to confirm before turning
    // them off ("socket, heater"), "off" for none
    read_str_or("Confirm_off", g_sys_param.confirm_off, sizeof(g_sys_param.confirm_off), CONFIG_ASSISTANT_CONFIRM_OFF,
                true);
    if (is_on(g_sys_param.confirm_off, &on) && !on) {
        g_sys_param.confirm_off[0] = '\0';
    }

    // Memory: free text, edited in MEMORY.TXT
    free(g_sys_param.memory);
    g_sys_param.memory = read_text(my_handle, "Memory");

    nvs_close(my_handle);

    // The room left for the settings (the MCP servers of CONFIG.INI take some)
    nvs_stats_t stats;
    if (nvs_get_stats(uf2_nvs_partition, &stats) == ESP_OK) {
        ESP_LOGI(TAG, "NVS: %u of %u entries used, %u free", (unsigned)stats.used_entries,
                 (unsigned)stats.total_entries, (unsigned)stats.free_entries);
    }
    ESP_LOGI(TAG, "stored ssid:%s", g_sys_param.ssid);
    ESP_LOGI(TAG, "stored password: (%d chars)", (int)strlen(g_sys_param.password));
    ESP_LOGI(TAG, "stored OpenAI key: (%d chars)", (int)strlen(g_sys_param.key));
    ESP_LOGI(TAG, "stored Base URL:%s", g_sys_param.url);
    ESP_LOGI(TAG, "volume:%d, country:%s, city:%s", g_sys_param.volume, g_sys_param.country, g_sys_param.city);
    ESP_LOGI(TAG, "voice:%s, memory: %d chars", g_sys_param.voice,
             g_sys_param.memory ? (int)strlen(g_sys_param.memory) : 0);
    ESP_LOGI(TAG, "language:%s, answer scrolling: %d characters/s, %s", g_sys_param.lang, g_sys_param.scroll_cps,
             g_sys_param.scroll_smooth ? "smooth" : "line by line");
    ESP_LOGI(TAG, "chat model:%s, Claude key: (%d chars), Gemini key: (%d chars), OpenRouter key: (%d chars), "
             "xAI key: (%d chars)", g_sys_param.chat_model, (int)strlen(g_sys_param.claude_key),
             (int)strlen(g_sys_param.gemini_key), (int)strlen(g_sys_param.openrouter_key),
             (int)strlen(g_sys_param.xai_key));
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

/* Kept by the soft reset: Wi-Fi, API keys, MEMORY.TXT, comments of CONFIG.INI; the MCP servers (Homey) too */
static const char *const s_kept_keys[] = {
    "ssid", "password", "ChatGPT_key", "Claude_key", "Gemini_key", "OpenRouter_key", "XAI_key",
    "Mistral_key", "Perplexity_key", "Deepgram_key", "Groq_key", "ElevenLabs_key",
    "Google_client", "Google_secret", "Memory", "Config_layout",
};

static bool is_kept_key(const char *key)
{
    // Mcp3_url, Mcp3_key, Mcp3_name, Mcp3_tools (Mcp3_on, switched on or off on the MCP page, goes back to on)
    size_t len = strlen(key);
    if (strncmp(key, "Mcp", 3) == 0 && isdigit((unsigned char)key[3]) && !(len > 3 && strcmp(key + len - 3, "_on") == 0)) {
        return true;
    }
    for (size_t i = 0; i < sizeof(s_kept_keys) / sizeof(s_kept_keys[0]); i++) {
        if (strcmp(key, s_kept_keys[i]) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t settings_factory_wipe(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open_from_partition(uf2_nvs_partition, uf2_nvs_namespace, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs open failed (0x%x)", ret);
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

char *settings_read_value(const char *key)
{
    nvs_handle_t handle;
    if (nvs_open_from_partition(uf2_nvs_partition, uf2_nvs_namespace, NVS_READONLY, &handle) != ESP_OK) {
        return NULL;
    }
    char *value = read_text(handle, key);
    nvs_close(handle);
    return value;
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
