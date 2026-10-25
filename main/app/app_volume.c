/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdbool.h>
#include <stdio.h>
#include "esp_log.h"
#include "bsp_board.h"
#include "settings.h"
#include "app_volume.h"

static const char *TAG = "volume";

static int s_level = 0;
static volatile int s_pending = -1;
static volatile bool s_unsaved = false;

void app_volume_init(int level)
{
    s_level = level;
    bsp_codec_volume_set(level, NULL);
    ESP_LOGI(TAG, "volume %d", level);
}

int app_volume_get(void)
{
    int pending = s_pending;
    return pending >= 0 ? pending : s_level;
}

int app_volume_request(int level)
{
    level = level < 0 ? 0 : (level > 100 ? 100 : level);
    s_pending = level;
    return level;
}

void app_volume_apply_pending(void)
{
    int pending = s_pending;
    if (pending < 0) {
        return;
    }
    s_pending = -1;
    if (pending != s_level) {
        s_level = pending;
        bsp_codec_volume_set(pending, NULL);
        s_unsaved = true;
        ESP_LOGI(TAG, "volume set to %d", pending);
    }
}

void app_volume_save(void)
{
    if (!s_unsaved) {
        return;
    }
    s_unsaved = false;
    char value[8];
    snprintf(value, sizeof(value), "%d", s_level);
    settings_save_str("Volume", value);
}
