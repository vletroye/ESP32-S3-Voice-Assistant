/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Screen switched off when nobody is there: with the ESP32-S3-BOX-3-SENSOR dock, its AT581X radar (24 GHz)
 * tells when someone is near. After Screen_off_s seconds without presence, while the assistant sleeps, the
 * backlight goes off; a presence, the wake word or a touch switches it on again. Only the backlight: the audio
 * and the wake word keep running. Without the dock (no radar on its I2C bus), the screen stays on.
 * The screen closes like an iris (a black ring growing from the edges to the centre, the backlight fading out
 * meanwhile) and opens again from the centre, faster; a presence while it closes opens it again at once.
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
/* Iris: a circle covering the whole screen (320 x 240: 400 px of diagonal), its black border growing inwards */
#define IRIS_DIAMETER           420
#define IRIS_STEP_MS            40
#define IRIS_CLOSE_STEPS        15      /* 0.6 s */
#define IRIS_OPEN_STEPS         8       /* 0.3 s */
/* Two refreshes of the display: the iris is on screen before the backlight goes on */
#define IRIS_DRAWN_MS           60

static const char *TAG = "presence";

static int s_timeout_s = 0;
static volatile int64_t s_last_activity_us = 0;
static bool s_screen_off = false;
static TaskHandle_t s_task = NULL;
static lv_obj_t *s_iris = NULL;     /* on the top layer while the screen closes, is off, or opens */

/* Nobody there for Screen_off_s (radar, touch, app_presence_activity()), while the assistant sleeps on its clock
 * (not during an answer, the Settings or welcome screen) */
static bool idle_now(void)
{
    int64_t now = esp_timer_get_time();
    if (gpio_get_level(BSP_RADAR_OUT_IO) || lv_disp_get_inactive_time(NULL) < PRESENCE_TOUCH_MS) {
        s_last_activity_us = now;
    }
    return now - s_last_activity_us > (int64_t)s_timeout_s * 1000000 && ui_ctrl_screen_can_sleep();
}

static void iris_create(void)
{
    if (!s_iris) {
        s_iris = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_iris);
        lv_obj_clear_flag(s_iris, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_color(s_iris, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_border_opa(s_iris, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_iris, lv_color_black(), LV_PART_MAIN);
    }
}

/* Border of the iris: 0 open, IRIS_DIAMETER / 2 closed (a ring: costly to draw, only while it moves) */
static void iris_set(int border)
{
    bsp_display_lock(0);
    iris_create();
    lv_obj_set_size(s_iris, IRIS_DIAMETER, IRIS_DIAMETER);
    lv_obj_center(s_iris);
    lv_obj_set_style_radius(s_iris, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_iris, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_iris, border, LV_PART_MAIN);
    bsp_display_unlock();
}

/*
 * Closed for good, while the screen is off: a plain black rectangle, cheap to draw. A closed ring would be drawn
 * again at each frame of the robot's animations underneath, taking the CPU from the network.
 */
static void iris_closed(void)
{
    bsp_display_lock(0);
    iris_create();
    lv_obj_set_size(s_iris, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s_iris);
    lv_obj_set_style_radius(s_iris, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_iris, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_iris, LV_OPA_COVER, LV_PART_MAIN);
    bsp_display_unlock();
}

static void iris_remove(void)
{
    bsp_display_lock(0);
    if (s_iris) {
        lv_obj_del(s_iris);
        s_iris = NULL;
    }
    bsp_display_unlock();
}

/* Opened from the centre, from the border given (closed: IRIS_DIAMETER / 2) */
static void screen_open(int from_border)
{
    // The iris drawn before the backlight goes on (by the LVGL task: drawing here, lv_refr_now(), would overflow
    // this small stack): nothing else is seen first
    iris_set(from_border);
    vTaskDelay(pdMS_TO_TICKS(IRIS_DRAWN_MS));
    bsp_display_backlight_on();
    for (int i = 1; i <= IRIS_OPEN_STEPS; i++) {
        iris_set(from_border * (IRIS_OPEN_STEPS - i) / IRIS_OPEN_STEPS);
        vTaskDelay(pdMS_TO_TICKS(IRIS_STEP_MS));
    }
    iris_remove();
    s_screen_off = false;
    ESP_LOGI(TAG, "screen on");
}

/* Closed from the edges to the centre while the backlight fades out; someone back meanwhile opens it again */
static void screen_close(void)
{
    const int closed = IRIS_DIAMETER / 2;
    for (int i = 1; i <= IRIS_CLOSE_STEPS; i++) {
        // Ease in: the edges go first, slowly, the centre last
        int border = closed * i * i / (IRIS_CLOSE_STEPS * IRIS_CLOSE_STEPS);
        if (!idle_now()) {
            screen_open(border);
            return;
        }
        iris_set(border);
        bsp_display_brightness_set(100 - 100 * i * i / (IRIS_CLOSE_STEPS * IRIS_CLOSE_STEPS));
        vTaskDelay(pdMS_TO_TICKS(IRIS_STEP_MS));
    }
    bsp_display_backlight_off();
    // Black while the screen is off: nothing else shows first when the backlight goes on again
    iris_closed();
    s_screen_off = true;
    ESP_LOGI(TAG, "screen off (nobody there)");
}

void app_presence_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
    if (s_task) {
        xTaskNotifyGive(s_task);     // the screen opens at once
    }
}

static void presence_task(void *arg)
{
    for (;;) {
        // Every PRESENCE_POLL_MS, or at once on app_presence_activity()
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PRESENCE_POLL_MS));
        bool idle = idle_now();
        if (idle && !s_screen_off) {
            screen_close();
        } else if (!idle && s_screen_off) {
            screen_open(IRIS_DIAMETER / 2);
        }
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
    // Small task, internal RAM (I2C, GPIO and the iris object of LVGL)
    if (xTaskCreatePinnedToCore(presence_task, "presence", 3 * 1024, NULL, 2, &s_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "No presence task: the screen stays on");
        return;
    }
    ESP_LOGI(TAG, "Presence radar found: the screen goes off after %d s without presence", s_timeout_s);
}
