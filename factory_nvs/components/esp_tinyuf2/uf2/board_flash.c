/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Ha Thach for Adafruit Industries
 * Copyright (c) Espressif
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "board_flash.h"
#include "nvs.h"
#include "esp_inih.h"
#include "esp_tinyuf2.h"
#include "esp_check.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>

#define TAG                   "uf2"
#define PRINTF(...)           ESP_LOGI(TAG, __VA_ARGS__)
#define PRINTFE(...)          ESP_LOGE(TAG, __VA_ARGS__)
#define PRINTFD(...)          ESP_LOGD(TAG, __VA_ARGS__)
#define PRINT_LOCATION()      ESP_LOGI(TAG, "%s: %d", __PRETTY_FUNCTION__, __LINE__)
#define PRINT_MESS(x)         ESP_LOGI(TAG, "%s", (char*)(x))
#define PRINT_STR(x)          ESP_LOGI(TAG, #x " = %s"   , (char*)(x) )
#define PRINT_INT(x)          ESP_LOGI(TAG, #x " = %d"  , (int32_t) (x) )
#define PRINT_HEX(x)          ESP_LOGI(TAG, #x " = 0x%X", (uint32_t) (x) )

#define PRINT_BUFFER(buf, n) \
  do {\
    uint8_t const* p8 = (uint8_t const*) (buf);\
    printf(#buf ": ");\
    for(uint32_t i=0; i<(n); i++) printf("%x ", p8[i]);\
    printf("\n");\
  }while(0)

static uint32_t _fl_addr = FLASH_CACHE_INVALID_ADDR;
static uint8_t *_fl_buf = NULL;
static bool _if_restart = false;
static update_complete_cb_t _complete_cb = NULL;
static esp_partition_t const* _part_ota = NULL;
char *_ini_file = NULL;
char *_ini_file_dummy = NULL;

/*
 * [Voice assistant] PERSONA.TXT and MEMORY.TXT; the help is shown in an empty file. '#' lines are saved with the
 * text (they stay in the file) and left out by the assistant.
 */
typedef struct {
    const char *key;        /* NVS key, NULL for a file of SPIFFS */
    const char *help;
    char *content;
    char *dummy;
    char path[40];          /* SPIFFS file ("" for an NVS one) */
    char fat_name[12];      /* its name on the drive, 8.3 without the dot ("VOICE_FRTXT") */
} text_file_t;

static text_file_t s_text_files[UF2_TEXT_COUNT] = {
    [UF2_TEXT_PERSONA] = {
        .key = "Persona",
        .help = "# Decrivez ici l'assistant et vous-meme (nom, createur, qui vous etes...).\r\n"
                "# Ce texte est donne a l'assistant avec chaque question. Plusieurs lignes possibles.\r\n"
                "# Describe the assistant and yourself here; this text is given to it with every question.\r\n"
                "# Les lignes commencant par # sont des commentaires : conservees, mais pas lues par l'assistant.\r\n"
                "# Lines starting with # are comments: kept, but not read by the assistant.\r\n",
    },
    [UF2_TEXT_MEMORY] = {
        .key = "Memory",
        .help = "# Ce que vous avez demande a l'assistant de retenir (\"souviens-toi que ...\").\r\n"
                "# Une note par ligne ; vous pouvez en corriger ou en supprimer.\r\n"
                "# What you asked the assistant to remember (\"remember that ...\"), one note per line.\r\n"
                "# Les lignes commencant par # sont des commentaires : conservees, mais pas lues par l'assistant.\r\n"
                "# Lines starting with # are comments: kept, but not read by the assistant.\r\n",
    },
};

const char *board_flash_text_fat_name(int text)
{
    return s_text_files[text].fat_name[0] ? s_text_files[text].fat_name : NULL;
}

/* The phrase files of the voice commands found on SPIFFS ("phrases_fr.txt" -> VOICE_FR.TXT) */
#define PHRASES_DIR     "/spiffs"
#define PHRASES_PREFIX  "phrases_"
#define PHRASES_SUFFIX  ".txt"

static void phrase_files_find(void)
{
    DIR *dir = opendir(PHRASES_DIR);
    if (!dir) {
        PRINTFE("SPIFFS not mounted: no phrase files");
        return;
    }
    int slot = UF2_TEXT_PHRASES;
    // The models offered (AGENTS.INI) first, when the assistant installed it
    FILE *agents = fopen(PHRASES_DIR "/agents.ini", "r");
    if (agents) {
        fclose(agents);
        text_file_t *t = &s_text_files[slot++];
        strlcpy(t->path, PHRASES_DIR "/agents.ini", sizeof(t->path));
        memcpy(t->fat_name, "AGENTS  INI", 12);
        PRINTF("%s shown as AGENTS.INI", t->path);
    }
    // CONFIG.INI saved by the "restore" icon of the Settings screen, when there is one
    FILE *backup = fopen(PHRASES_DIR "/config.bkp", "r");
    if (backup) {
        fclose(backup);
        text_file_t *t = &s_text_files[slot++];
        strlcpy(t->path, PHRASES_DIR "/config.bkp", sizeof(t->path));
        memcpy(t->fat_name, "CONFIG  BKP", 12);
        PRINTF("%s shown as CONFIG.BKP", t->path);
    }
    // Results of the model test ("teste les modèles"), when there are some
    FILE *speed = fopen(PHRASES_DIR "/speed.txt", "r");
    if (speed) {
        fclose(speed);
        text_file_t *t = &s_text_files[slot++];
        strlcpy(t->path, PHRASES_DIR "/speed.txt", sizeof(t->path));
        memcpy(t->fat_name, "SPEED   TXT", 12);
        PRINTF("%s shown as SPEED.TXT", t->path);
    }
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && slot < UF2_TEXT_COUNT) {
        size_t len = strlen(e->d_name);
        size_t code_len = len - strlen(PHRASES_PREFIX) - strlen(PHRASES_SUFFIX);
        if (len <= strlen(PHRASES_PREFIX) + strlen(PHRASES_SUFFIX) || code_len > 3 ||
                strncmp(e->d_name, PHRASES_PREFIX, strlen(PHRASES_PREFIX)) != 0 ||
                strcmp(e->d_name + len - strlen(PHRASES_SUFFIX), PHRASES_SUFFIX) != 0) {
            continue;
        }
        text_file_t *t = &s_text_files[slot++];
        snprintf(t->path, sizeof(t->path), PHRASES_DIR "/%s", e->d_name);
        // "VOICE_" + code (upper case) padded to 8, then "TXT"
        char base[9] = "VOICE_";
        for (size_t i = 0; i < code_len && 6 + i < 8; i++) {
            base[6 + i] = toupper((unsigned char)e->d_name[strlen(PHRASES_PREFIX) + i]);
        }
        snprintf(t->fat_name, sizeof(t->fat_name), "%-8.8sTXT", base);
        PRINTF("%s shown as %.8s.TXT", t->path, t->fat_name);
    }
    closedir(dir);
}

/* A phrase file read from SPIFFS into its buffer */
static void phrase_file_read(text_file_t *t)
{
    FILE *f = fopen(t->path, "r");
    size_t n = f ? fread(t->content, 1, CFG_UF2_TEXT_FILE_SIZE - 1, f) : 0;
    if (f) {
        fclose(f);
    }
    t->content[n] = '\0';
}

char *board_flash_text_content(int text)
{
    return s_text_files[text].content;
}

char *board_flash_text_dummy(int text)
{
    return s_text_files[text].dummy;
}

static text_file_t *text_file_of_key(const char *key)
{
    for (int i = 0; i < UF2_TEXT_COUNT; i++) {
        if (s_text_files[i].key && strcmp(key, s_text_files[i].key) == 0) {
            return &s_text_files[i];
        }
    }
    return NULL;
}
static nvs_modified_cb_t _modified_cb = NULL;
static char _part_name[16] = "";
static char _namespace_name[16] = "";
#ifdef CONFIG_UF2_INI_NVS_VALUE_HIDDEN
char *HIDDEN_STR[CONFIG_UF2_INI_NVS_HIDDEN_MAX_NUM];
size_t hidden_str_num = 0;
bool if_all_hidden = false;
#endif

uint8_t board_usb_get_serial(uint8_t serial_id[16])
{
    // use factory default MAC as serial ID
    esp_efuse_mac_get_default(serial_id);
    return 6;
}

void board_dfu_complete(void)
{
    esp_ota_set_boot_partition(_part_ota);

    if (_complete_cb) {
        PRINTF("dfu_complete: run user callback");
        _complete_cb();
    }

    if (_if_restart) {
        /* code */
        esp_tinyuf2_uninstall();
        PRINTF("UF2 Flashing complete, restart...");
        esp_restart();
    }
}

void board_flash_init(esp_partition_subtype_t subtype, const char *label, update_complete_cb_t complete_cb, bool if_restart)
{
    _fl_addr = FLASH_CACHE_INVALID_ADDR;
    _if_restart = if_restart;
    _complete_cb = complete_cb;

    if (subtype == ESP_PARTITION_SUBTYPE_ANY) {
        _part_ota = esp_ota_get_next_update_partition(NULL);
    } else {
        _part_ota = esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, label);
    }

    if (_part_ota == NULL) {
        PRINTFE("Partition argument not found in partition table");
        assert(0);
    }
#ifndef CONFIG_UF2_OTA_FACTORY_ONLY
    esp_partition_t const* runing_ota_ptt = esp_ota_get_running_partition();
    if (runing_ota_ptt == _part_ota) {
        PRINTFE("Can not write to running partition");
        assert(0);
    }
#endif
    _fl_buf = heap_caps_malloc(FLASH_CACHE_SIZE, MALLOC_CAP_32BIT);
    if (_fl_buf == NULL || (uint32_t)_fl_buf % 4 != 0) {
        PRINTFE("Can not allocate memory for flash cache");
        assert(0);
    }
}

void board_flash_deinit(void)
{
    if (_fl_buf) {
        free(_fl_buf);
        _fl_buf = NULL;
    }
}

static void ini_insert_section(const char *section)
{
    if (section == NULL || _ini_file == NULL) {
        return;
    }
    size_t len = strlen(_ini_file);
    snprintf(_ini_file + len, CFG_UF2_INI_FILE_SIZE - len, "[%s]\r\n", section);
}

/*
 * [Voice assistant] CONFIG.INI as last written by the PC (comments, blank lines, order), kept in this NVS key:
 * the file is rebuilt from it with the current values, so that comments survive. Not shown in the file.
 */
#define INI_LAYOUT_KEY          "Config_layout"
#define INI_LAYOUT_MAX_LEN      3999    /* NVS string limit */
#define INI_LAYOUT_MAX_KEYS     64

/* A line as is (comment, section, blank) */
static void ini_insert_line(const char *line, size_t len)
{
    size_t used = strlen(_ini_file);
    if (used + len + 3 >= CFG_UF2_INI_FILE_SIZE) {
        PRINTFE("CONFIG.INI full: line left out");
        return;
    }
    memcpy(_ini_file + used, line, len);
    memcpy(_ini_file + used + len, "\r\n", 3);
}

static void ini_insert_pair(const char *key, const char *value)
{
    if (key == NULL || value == NULL || _ini_file == NULL) {
        return;
    }
    size_t len = strlen(_ini_file);
    size_t needed = strlen(key) + strlen(value) + 5;
    if (len + needed >= CFG_UF2_INI_FILE_SIZE) {
        PRINTFE("CONFIG.INI full: key '%s' left out", key);
        return;
    }
    snprintf(_ini_file + len, CFG_UF2_INI_FILE_SIZE - len, "%s = %s\r\n", key, value);
}

#ifdef CONFIG_UF2_INI_NVS_VALUE_HIDDEN
esp_err_t esp_tinyuf2_set_all_key_hidden(bool if_hidden)
{
    if_all_hidden = if_hidden;
    return ESP_OK;
}

esp_err_t esp_tinyuf2_add_key_hidden(const char *key)
{
    ESP_RETURN_ON_FALSE(key != NULL, ESP_ERR_INVALID_ARG, TAG, "key is NULL");
    ESP_RETURN_ON_FALSE(hidden_str_num < CONFIG_UF2_INI_NVS_HIDDEN_MAX_NUM, ESP_ERR_NO_MEM, TAG, "hidden_str_num >= CONFIG_UF2_INI_NVS_HIDDEN_MAX_NUM");
    hidden_str[hidden_str_num] = (char *)malloc(strlen(key) + 1);
    ESP_RETURN_ON_FALSE(hidden_str[hidden_str_num ] != NULL, ESP_ERR_NO_MEM, TAG, "malloc failed");
    strcpy(hidden_str[hidden_str_num], key);
    hidden_str_num++;
    return ESP_OK;
}

static bool check_value_if_hidden(const char *key)
{
    ESP_RETURN_ON_FALSE(key != NULL, false, TAG, "key is NULL");
    if (if_all_hidden) {
        return true;
    }
    for (int i = 0; i < hidden_str_num; i++) {
        if (strcmp(key, hidden_str[i]) == 0) {
            return true;
        }
    }
    return false;
}
#endif

static bool is_blank(char c)
{
    return c == ' ' || c == '\t';
}

/*
 * The saved layout with the current values: "key = value ; comment" lines get the value stored in NVS (changed
 * meanwhile by voice or by the assistant), keep their inline comment, and are dropped when the key no longer
 * exists. The keys written are listed in `used`.
 */
static void ini_gen_from_layout(nvs_handle_t nvs, const char *layout, char used[][NVS_KEY_NAME_MAX_SIZE],
                                int *used_count)
{
    for (const char *line = layout; *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        const char *next = line + len + (end ? 1 : 0);
        while (len > 0 && line[len - 1] == '\r') {
            len--;
        }
        const char *p = line;
        while (p < line + len && is_blank(*p)) {
            p++;
        }
        const char *eq = memchr(p, '=', line + len - p);
        if (p == line + len || *p == '#' || *p == ';' || *p == '[' || !eq) {
            ini_insert_line(line, len);
            line = next;
            continue;
        }
        // Key: before '=', without the blanks around it
        const char *key_end = eq;
        while (key_end > p && is_blank(key_end[-1])) {
            key_end--;
        }
        char key[NVS_KEY_NAME_MAX_SIZE];
        size_t key_len = (size_t)(key_end - p);
        if (key_len == 0 || key_len >= sizeof(key)) {
            ini_insert_line(line, len);
            line = next;
            continue;
        }
        memcpy(key, p, key_len);
        key[key_len] = '\0';
        // Inline comment: from the blank before a ';' (as the INI parser reads it)
        const char *comment = NULL;
        for (const char *c = eq + 1; c < line + len; c++) {
            if (*c == ';' && is_blank(c[-1])) {
                comment = c - 1;
                while (comment > eq + 1 && is_blank(comment[-1])) {
                    comment--;
                }
                break;
            }
        }
        size_t value_len = 0;
        if (strcmp(key, INI_LAYOUT_KEY) == 0 || text_file_of_key(key) ||
                nvs_get_str(nvs, key, NULL, &value_len) != ESP_OK) {
            line = next;    // no longer stored: left out
            continue;
        }
        char *value = (char *)malloc(value_len);
        if (value && nvs_get_str(nvs, key, value, &value_len) == ESP_OK) {
            size_t used_len = strlen(_ini_file);
            size_t comment_len = comment ? (size_t)(line + len - comment) : 0;
            if (used_len + key_len + strlen(value) + comment_len + 6 < CFG_UF2_INI_FILE_SIZE) {
                snprintf(_ini_file + used_len, CFG_UF2_INI_FILE_SIZE - used_len, "%s = %s%.*s\r\n", key,
#ifdef CONFIG_UF2_INI_NVS_VALUE_HIDDEN
                         check_value_if_hidden(key) ? "****" : value,
#else
                         value,
#endif
                         (int)comment_len, comment ? comment : "");
            } else {
                PRINTFE("CONFIG.INI full: key '%s' left out", key);
            }
            if (*used_count < INI_LAYOUT_MAX_KEYS) {
                strlcpy(used[(*used_count)++], key, NVS_KEY_NAME_MAX_SIZE);
            }
        }
        free(value);
        line = next;
    }
}

/* [Voice assistant] Order of the keys in CONFIG.INI (when not set by the file as last written); others follow */
static const char *const s_ini_key_order[] = {
    "Base_url", "Chat_model", "Voice", "ssid", "password", "Scroll_speed", "Scroll_mode", "Language", "Country",
    "City", "Volume", "ChatGPT_key", "Claude_key", "Gemini_key", "Homey_url", "Homey_key", "OpenRouter_key",
    "OpenRouter_free", "Free_models",
    "Screen_off_s", "End_speech_ms",
};

/* The keys of s_ini_key_order not written yet (from the layout), in that order */
static void ini_gen_ordered(nvs_handle_t nvs, char used[][NVS_KEY_NAME_MAX_SIZE], int *used_count)
{
    for (size_t k = 0; k < sizeof(s_ini_key_order) / sizeof(s_ini_key_order[0]); k++) {
        const char *key = s_ini_key_order[k];
        bool done = false;
        for (int i = 0; i < *used_count && !done; i++) {
            done = (strcmp(key, used[i]) == 0);
        }
        size_t len = 0;
        if (done || *used_count >= INI_LAYOUT_MAX_KEYS || nvs_get_str(nvs, key, NULL, &len) != ESP_OK) {
            continue;
        }
        char *value = (char *)malloc(len);
        if (value && nvs_get_str(nvs, key, value, &len) == ESP_OK) {
#ifdef CONFIG_UF2_INI_NVS_VALUE_HIDDEN
            ini_insert_pair(key, check_value_if_hidden(key) ? "****" : value);
#else
            ini_insert_pair(key, value);
#endif
            strlcpy(used[(*used_count)++], key, NVS_KEY_NAME_MAX_SIZE);
        }
        free(value);
    }
}

static void ini_gen_from_nvs(const char *part, const char *name)
{
    if (part == NULL || name == NULL || _ini_file == NULL) {
        return;
    }
    nvs_iterator_t it = NULL;
    nvs_handle_t nvs = 0;
    esp_err_t result;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    result = nvs_entry_find(part, name, NVS_TYPE_STR, &it);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        PRINTFE("No such namespace entry was found");
        return;
    }

    if (result != ESP_OK) {
        PRINTFE("NVS find error: %s", esp_err_to_name(result));
        return;
    }
#else
    it = nvs_entry_find(part, name, NVS_TYPE_STR);
    if (it == NULL) {
        PRINTFE("No such namespace entry was found");
        return;
    }
#endif

    result = nvs_open_from_partition(part, name, NVS_READONLY, &nvs);
    if (result != ESP_OK) {
        PRINTFE("NVS open error: %s", esp_err_to_name(result));
        assert(0); //should not happen
    }

    // [Voice assistant] The file as last written (comments included), then the keys in s_ini_key_order, then
    // any other one in NVS order
    static char used[INI_LAYOUT_MAX_KEYS][NVS_KEY_NAME_MAX_SIZE];
    int used_count = 0;
    size_t layout_len = 0;
    char *layout = NULL;
    if (nvs_get_str(nvs, INI_LAYOUT_KEY, NULL, &layout_len) == ESP_OK && (layout = (char *)malloc(layout_len)) &&
            nvs_get_str(nvs, INI_LAYOUT_KEY, layout, &layout_len) == ESP_OK) {
        ini_gen_from_layout(nvs, layout, used, &used_count);
    } else {
        ini_insert_section(name);
    }
    free(layout);
    ini_gen_ordered(nvs, used, &used_count);

    do {
        nvs_entry_info_t info;
        size_t len = 0;
        nvs_entry_info(it, &info);
        if ((result = nvs_get_str(nvs, info.key, NULL, &len)) == ESP_OK) {
            char *str = (char *)malloc(len);
            text_file_t *text_file = text_file_of_key(info.key);
            if ((result = nvs_get_str(nvs, info.key, str, &len)) == ESP_OK && text_file) {
                // [Voice assistant] Persona and Memory go to their own files, not to CONFIG.INI
                if (str[0] && text_file->content) {
                    strlcpy(text_file->content, str, CFG_UF2_TEXT_FILE_SIZE);
                }
            } else if (result == ESP_OK) {
                // [Voice assistant] Already written from the layout, or the layout itself
                bool skip = (strcmp(info.key, INI_LAYOUT_KEY) == 0);
                for (int i = 0; i < used_count && !skip; i++) {
                    skip = (strcmp(info.key, used[i]) == 0);
                }
                if (skip) {
                    free(str);
                    goto next;
                }
#ifdef CONFIG_UF2_INI_NVS_VALUE_HIDDEN
                if (!check_value_if_hidden(info.key)) {
                    ini_insert_pair(info.key, "****");
                } else
#endif
                {
                    ini_insert_pair(info.key, str);
                }
                PRINTFD("Add namespace '%s', key '%s', value '%s' \n",
                        name, info.key, str);
            }
            free(str);
        }
next:
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        result = nvs_entry_next(&it);
    } while (result == ESP_OK);
#else
        it = nvs_entry_next(it);
    } while (it != NULL);
#endif
    strcpy(_ini_file_dummy, _ini_file);
    for (int i = 0; i < UF2_TEXT_COUNT; i++) {
        text_file_t *t = &s_text_files[i];
        if (t->content && t->path[0]) {
            phrase_file_read(t);
        }
        if (t->content) {
            if (t->content[0] == '\0' && t->help) {
                strlcpy(t->content, t->help, CFG_UF2_TEXT_FILE_SIZE);
            }
            memcpy(t->dummy, t->content, CFG_UF2_TEXT_FILE_SIZE);
        }
    }
    nvs_close(nvs);
    nvs_release_iterator(it);
}

void board_flash_text_update(int index)
{
    text_file_t *t = &s_text_files[index];
    // The written text ends at the first NUL (the end of the last sector is zero-filled)
    t->dummy[CFG_UF2_TEXT_FILE_SIZE - 1] = '\0';
    memcpy(t->content, t->dummy, CFG_UF2_TEXT_FILE_SIZE);

    if (t->path[0]) {
        // A phrase file: saved as written (emptied, the assistant installs its original version again)
        FILE *f = fopen(t->path, "w");
        size_t len = strlen(t->dummy);
        bool ok = f && fwrite(t->dummy, 1, len, f) == len;
        if (f) {
            ok &= (fclose(f) == 0);
        }
        if (ok) {
            ESP_LOGI(TAG, "%s saved (%u bytes)", t->path, (unsigned)len);
            if (_modified_cb) {
                _modified_cb();
            }
        } else {
            ESP_LOGE(TAG, "%s not saved", t->path);
        }
        return;
    }

    // Saved with the '#' lines (comments, left out by the assistant), without trailing spaces, within the NVS
    // string limit
    char *text = (char *)calloc(1, CFG_UF2_TEXT_FILE_SIZE);
    if (text == NULL) {
        return;
    }
    size_t n = strlen(t->dummy);
    memcpy(text, t->dummy, n);
    while (n > 0 && (unsigned char)text[n - 1] <= ' ') {
        n--;
    }
    if (n > CFG_UF2_TEXT_MAX_LEN) {
        n = CFG_UF2_TEXT_MAX_LEN;
        while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) {
            n--;    // do not cut a UTF-8 character
        }
    }
    text[n] = '\0';

    nvs_handle_t nvs = 0;
    if (nvs_open_from_partition(_part_name, _namespace_name, NVS_READWRITE, &nvs) == ESP_OK) {
        char *old = (char *)calloc(1, CFG_UF2_TEXT_FILE_SIZE);
        size_t old_len = CFG_UF2_TEXT_FILE_SIZE;
        bool changed = !old || nvs_get_str(nvs, t->key, old, &old_len) != ESP_OK || strcmp(old, text) != 0;
        free(old);
        if (changed && nvs_set_str(nvs, t->key, text) == ESP_OK && nvs_commit(nvs) == ESP_OK) {
            ESP_LOGI(TAG, "%s saved (%u bytes)", t->key, (unsigned)n);
            if (_modified_cb) {
                _modified_cb();
            }
        }
        nvs_close(nvs);
    }
    free(text);
}

static int nvs_write_back(void* user, const char* section, const char* name,
                          const char* value)
{
    nvs_handle_t nvs = (nvs_handle_t)user;
    if (strcmp(name, INI_LAYOUT_KEY) == 0) {
        return 1;   // [Voice assistant] reserved
    }
    PRINTFD("... [%s]", section);
    PRINTFD("... (%s=%s)", name, value);
    ESP_ERROR_CHECK(nvs_set_str(nvs, name, value));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    return 1;
}

static void ini_parse_to_nvs(const char *ini_str)
{
    //we currently only support string type
    PRINTFD("ini_parse_to_nvs: \n%s", ini_str);
    nvs_handle_t nvs = 0;
    esp_err_t result = nvs_open_from_partition(_part_name, _namespace_name, NVS_READWRITE, &nvs);
    if (result != ESP_OK) {
        PRINTFE("NVS open error: %s", esp_err_to_name(result));
        assert(0); //should not happen
    }
    ini_parse_string(ini_str, nvs_write_back, (void*)nvs);
    // [Voice assistant] The file itself, comments included, to rebuild it the same way next time
    size_t len = strlen(ini_str);
    if (len <= INI_LAYOUT_MAX_LEN) {
        char *old = (char *)calloc(1, INI_LAYOUT_MAX_LEN + 1);
        size_t old_len = INI_LAYOUT_MAX_LEN + 1;
        if (!old || nvs_get_str(nvs, INI_LAYOUT_KEY, old, &old_len) != ESP_OK || strcmp(old, ini_str) != 0) {
            if (nvs_set_str(nvs, INI_LAYOUT_KEY, ini_str) == ESP_OK && nvs_commit(nvs) == ESP_OK) {
                PRINTF("CONFIG.INI layout saved (%u bytes)", (unsigned)len);
            }
        }
        free(old);
    } else {
        PRINTFE("CONFIG.INI too long (%u bytes) to keep its comments", (unsigned)len);
    }
    nvs_close(nvs);
}

void board_flash_nvs_init(const char *part_name, const char *namespace_name, nvs_modified_cb_t modified_cb)
{
    _modified_cb = modified_cb;
    strcpy(_namespace_name, namespace_name);
    strcpy(_part_name, part_name);
    if (_ini_file == NULL) {
        //TODO: free when support deinit
        _ini_file = (char *)calloc(1, CFG_UF2_INI_FILE_SIZE);
        _ini_file_dummy = (char *)calloc(1, CFG_UF2_INI_FILE_SIZE);
        phrase_files_find();
        for (int i = 0; i < UF2_TEXT_COUNT; i++) {
            if (!s_text_files[i].key && !s_text_files[i].path[0]) {
                continue;   // unused phrase file slot
            }
            s_text_files[i].content = (char *)calloc(1, CFG_UF2_TEXT_FILE_SIZE);
            s_text_files[i].dummy = (char *)calloc(1, CFG_UF2_TEXT_FILE_SIZE);
        }
    }
    ini_gen_from_nvs(part_name, namespace_name);
}

void board_flash_nvs_deinit(void)
{
    if (_ini_file) {
        free(_ini_file);
        _ini_file = NULL;
    }
    if (_ini_file_dummy) {
        free(_ini_file_dummy);
        _ini_file_dummy = NULL;
    }
}

void board_flash_nvs_update(const char *ini_str)
{
    if (ini_str == NULL) {
        PRINTFE("ini_str is NULL");
        return;
    }
    ini_parse_to_nvs(ini_str);
    if (_modified_cb) {
        _modified_cb();
    }
}

uint32_t board_flash_size(void)
{
    if (_part_ota == NULL) {
        return 0;
    }
    return _part_ota->size;
}

void board_flash_read(uint32_t addr, void* buffer, uint32_t len)
{
    if (_part_ota == NULL || buffer == NULL || len == 0) {
        return;
    }
    esp_partition_read(_part_ota, addr, buffer, len);
}

void board_flash_flush(void)
{
    if (_fl_addr == FLASH_CACHE_INVALID_ADDR) {
        return;
    }

    // Check if contents already matched
    bool content_matches = true;
    uint32_t const verify_sz = 4096;
    uint8_t* verify_buf = malloc(verify_sz);

    for (uint32_t count = 0; count < FLASH_CACHE_SIZE; count += verify_sz) {
        board_flash_read(_fl_addr + count, verify_buf, verify_sz);
        if (0 != memcmp(_fl_buf + count, verify_buf, verify_sz)) {
            content_matches = false;
            break;
        }
    }
    free(verify_buf);

    // skip erase & write if content already matches
    if (!content_matches) {
        esp_partition_erase_range(_part_ota, _fl_addr, FLASH_CACHE_SIZE);
        esp_partition_write(_part_ota, _fl_addr, _fl_buf, FLASH_CACHE_SIZE);
    }

    _fl_addr = FLASH_CACHE_INVALID_ADDR;
}

void board_flash_write(uint32_t addr, void const *data, uint32_t len)
{
    if (_part_ota == NULL || data == NULL || len == 0) {
        return;
    }
    // Align write address with size of cache
    uint32_t new_addr = addr & ~(FLASH_CACHE_SIZE - 1);

    // New address comes, we need flush cache data to real flash
    if (new_addr != _fl_addr) {
        board_flash_flush();

        _fl_addr = new_addr;
        // cache origin data of new address, to prevent data loss
        board_flash_read(new_addr, _fl_buf, FLASH_CACHE_SIZE);
    }
    // write received date to cache
    memcpy(_fl_buf + (addr & (FLASH_CACHE_SIZE - 1)), data, len);
}
