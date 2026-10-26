/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Screen switched off when nobody is there: with the ESP32-S3-BOX-3-SENSOR dock, its AT581X radar (24 GHz)
 * tells when someone is near. After Screen_off_s seconds without presence, while the assistant sleeps, the
 * backlight goes off; a presence, the wake word or a touch switches it on again. Only the backlight: the audio
 * and the wake word keep running. Without the dock (no radar on its I2C bus), the screen stays on.
 */

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "bsp_board.h"
#include "at581x.h"
#include "lvgl.h"
#include "app_ui_ctrl.h"
#include "app_presence.h"

/* The dock's I2C bus: the port the board does not use */
#define PRESENCE_I2C_PORT       ((BSP_I2C_NUM == 1) ? I2C_NUM_0 : I2C_NUM_1)
#define PRESENCE_I2C_SPEED_HZ   400000
#define PRESENCE_POLL_MS        200
/* A touch on the screen this recent counts as activity */
#define PRESENCE_TOUCH_MS       (2 * PRESENCE_POLL_MS)

static const char *TAG = "presence";

static int s_timeout_s = 0;
static volatile int64_t s_last_activity_us = 0;
static bool s_screen_off = false;

static void screen_set(bool on)
{
    if (on == !s_screen_off) {
        return;
    }
    s_screen_off = !on;
    if (on) {
        bsp_display_backlight_on();
    } else {
        bsp_display_backlight_off();
    }
    ESP_LOGI(TAG, "screen %s", on ? "on" : "off (nobody there)");
}

void app_presence_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
    screen_set(true);
}

static void presence_task(void *arg)
{
    int prev_level = -1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PRESENCE_POLL_MS));
        int64_t now = esp_timer_get_time();
        int level = gpio_get_level(BSP_RADAR_OUT_IO);
        if (level != prev_level) {
            ESP_LOGD(TAG, "radar: %s", level ? "presence" : "nobody");
            prev_level = level;
        }
        if (level || lv_disp_get_inactive_time(NULL) < PRESENCE_TOUCH_MS) {
            s_last_activity_us = now;
        }
        // Off only while the assistant sleeps on its clock (not during an answer, the Settings or welcome screen)
        bool idle = now - s_last_activity_us > (int64_t)s_timeout_s * 1000000 && ui_ctrl_screen_can_sleep();
        screen_set(!idle);
    }
}

static bool radar_probe(void)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, AT581X_ADDRRES_0 | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    bool found = (i2c_master_cmd_begin(PRESENCE_I2C_PORT, cmd, pdMS_TO_TICKS(100)) == ESP_OK);
    i2c_cmd_link_delete(cmd);
    return found;
}

void app_presence_init(int screen_off_s)
{
    s_timeout_s = screen_off_s;
    s_last_activity_us = esp_timer_get_time();
    if (s_timeout_s <= 0) {
        ESP_LOGI(TAG, "Screen_off_s = 0: the screen stays on");
        return;
    }
    const i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BSP_I2C_EXPAND_SDA,
        .sda_pullup_en = GPIO_PULLUP_DISABLE,
        .scl_io_num = BSP_I2C_EXPAND_SCL,
        .scl_pullup_en = GPIO_PULLUP_DISABLE,
        .master.clk_speed = PRESENCE_I2C_SPEED_HZ,
    };
    if (i2c_param_config(PRESENCE_I2C_PORT, &conf) != ESP_OK ||
            i2c_driver_install(PRESENCE_I2C_PORT, conf.mode, 0, 0, 0) != ESP_OK) {
        ESP_LOGW(TAG, "Dock I2C bus not available: the screen stays on");
        return;
    }
    if (!radar_probe()) {
        ESP_LOGI(TAG, "No presence radar (ESP32-S3-BOX-3-SENSOR dock): the screen stays on");
        i2c_driver_delete(PRESENCE_I2C_PORT);
        return;
    }
    const at581x_default_cfg_t radar_cfg = ATH581X_INITIALIZATION_CONFIG();
    const at581x_i2c_config_t radar_conf = {
        .i2c_port = PRESENCE_I2C_PORT,
        .i2c_addr = AT581X_ADDRRES_0,
        .int_gpio_num = GPIO_NUM_NC,
        .def_conf = &radar_cfg,
    };
    at581x_dev_handle_t radar = NULL;
    if (at581x_new_sensor(&radar_conf, &radar) != ESP_OK) {
        ESP_LOGW(TAG, "Presence radar not set up: the screen stays on");
        return;
    }
    const gpio_config_t out = {
        .pin_bit_mask = 1ULL << BSP_RADAR_OUT_IO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&out);
    // Small task, internal RAM (I2C and GPIO only)
    if (xTaskCreatePinnedToCore(presence_task, "presence", 3 * 1024, NULL, 2, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "No presence task: the screen stays on");
        return;
    }
    ESP_LOGI(TAG, "Presence radar found: the screen goes off after %d s without presence", s_timeout_s);
}
