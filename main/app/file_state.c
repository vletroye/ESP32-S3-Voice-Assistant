/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * The LittleFS partition of the files written often: see file_state.h.
 */

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "file_state.h"

static const char *TAG = "state";

#define STATE_PARTITION     "state"
#define STATE_COPY_PIECE    4096

/* Kept on SPIFFS by an older firmware: moved here at the first start */
static const char *const s_moved[] = {
    "history.json", "costs.json", "model_times.json", "speech_times.json", "places.json",
};

/* A file of SPIFFS copied here (4 KB at a time, through internal RAM), then removed there; false on error */
static bool move_file(const char *name, char *piece)
{
    char from[48], to[48];
    snprintf(from, sizeof(from), "/spiffs/%s", name);
    snprintf(to, sizeof(to), STATE_DIR "/%s", name);
    FILE *in = fopen(from, "r");
    if (!in) {
        return true;    // nothing to move
    }
    FILE *probe = fopen(to, "r");
    if (probe) {
        // Already here (moved before): the old one only removed
        fclose(probe);
        fclose(in);
        remove(from);
        return true;
    }
    FILE *out = fopen(to, "w");
    bool ok = out != NULL;
    size_t total = 0, n;
    while (ok && (n = fread(piece, 1, STATE_COPY_PIECE, in)) > 0) {
        ok = fwrite(piece, 1, n, out) == n;
        total += n;
    }
    fclose(in);
    if (out) {
        ok &= fclose(out) == 0;
    }
    if (ok) {
        remove(from);
        ESP_LOGI(TAG, "%s moved from SPIFFS (%u bytes)", name, (unsigned)total);
    } else {
        remove(to);     // kept on SPIFFS, moved at the next start
        ESP_LOGE(TAG, "%s not moved", name);
    }
    return ok;
}

esp_err_t state_mount(void)
{
    int64_t t0 = esp_timer_get_time();
    esp_vfs_littlefs_conf_t conf = {
        .base_path = STATE_DIR,
        .partition_label = STATE_PARTITION,
        .format_if_mount_failed = true,     // the first time (a new partition)
    };
    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS partition \"%s\" not mounted (%s): flash the partition table", STATE_PARTITION,
                 esp_err_to_name(ret));
        return ret;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info(STATE_PARTITION, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted on " STATE_DIR " in %lld ms: %u of %u bytes used",
             (esp_timer_get_time() - t0) / 1000, (unsigned)used, (unsigned)total);
    char *piece = heap_caps_malloc(STATE_COPY_PIECE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    for (size_t i = 0; piece && i < sizeof(s_moved) / sizeof(s_moved[0]); i++) {
        move_file(s_moved[i], piece);
    }
    free(piece);
    return ESP_OK;
}

void state_erase(void)
{
    DIR *dir = opendir(STATE_DIR);
    struct dirent *e;
    while (dir && (e = readdir(dir)) != NULL) {
        char path[300];
        snprintf(path, sizeof(path), STATE_DIR "/%s", e->d_name);
        remove(path);
        ESP_LOGI(TAG, "%s removed", path);
    }
    if (dir) {
        closedir(dir);
    }
}
