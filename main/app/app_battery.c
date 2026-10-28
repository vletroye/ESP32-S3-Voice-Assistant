/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdbool.h>
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "app_presence.h"
#include "app_battery.h"

#define BATTERY_ADC_UNIT        ADC_UNIT_1
#define BATTERY_ADC_CHANNEL     ADC_CHANNEL_9       /* GPIO10 */
#define BATTERY_ADC_ATTEN       ADC_ATTEN_DB_6      /* up to ~1.75 V on the pin: 4.2 V / 4 is ~1.05 V */
#define BATTERY_DIVIDER         4.01f               /* measured with a multimeter on the dock (Home Assistant forum) */
#define BATTERY_SAMPLES         16
#define BATTERY_MIN_V           2.8f                /* outside of this range: no cell (or not read right) */
#define BATTERY_MAX_V           4.5f
#define BATTERY_HYSTERESIS_V    0.02f
#define BATTERY_SMOOTH          0.3f                /* weight of a new reading */

static const char *TAG = "battery";

/* Voltages from which 2, 3, 4 and 5 sections are lit (below the first: 1 section, critical) */
static const float s_thresholds[APP_BATTERY_SECTIONS - 1] = { 3.50f, 3.70f, 3.80f, 3.95f };

static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;
static bool s_failed = false;
static float s_volts = 0;
static int s_band = -1;         /* 0..4 (thresholds passed), -1 not known yet */

static bool adc_init(void)
{
    if (s_adc || s_failed) {
        return s_adc != NULL;
    }
    const adc_oneshot_unit_init_cfg_t unit = { .unit_id = BATTERY_ADC_UNIT };
    const adc_oneshot_chan_cfg_t channel = { .atten = BATTERY_ADC_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT };
    const adc_cali_curve_fitting_config_t cali = {
        .unit_id = BATTERY_ADC_UNIT,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK ||
            adc_oneshot_config_channel(s_adc, BATTERY_ADC_CHANNEL, &channel) != ESP_OK ||
            adc_cali_create_scheme_curve_fitting(&cali, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC not available: no battery level");
        s_failed = true;
        return false;
    }
    return true;
}

/* The battery's voltage, 0 when it cannot be read */
static float read_volts(void)
{
    int sum = 0, n = 0;
    for (int i = 0; i < BATTERY_SAMPLES; i++) {
        int raw = 0, mv = 0;
        if (adc_oneshot_read(s_adc, BATTERY_ADC_CHANNEL, &raw) == ESP_OK &&
                adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
            sum += mv;
            n++;
        }
    }
    return n ? (float)sum / n / 1000.0f * BATTERY_DIVIDER : 0;
}

int app_battery_sections(bool *critical)
{
    *critical = false;
    // Without the dock, GPIO10 is not connected (any value could be read)
    if (!app_presence_dock() || !adc_init()) {
        return 0;
    }
    float volts = read_volts();
    if (volts < BATTERY_MIN_V || volts > BATTERY_MAX_V) {
        if (s_band >= 0) {
            ESP_LOGI(TAG, "No battery (%d mV)", (int)(volts * 1000));    // no float: the LVGL task's stack
        }
        s_band = -1;
        s_volts = 0;
        return 0;
    }
    s_volts = (s_band < 0) ? volts : s_volts + BATTERY_SMOOTH * (volts - s_volts);
    // The thresholds passed, with a little hysteresis around the current one
    int band = 0;
    while (band < APP_BATTERY_SECTIONS - 1 && s_volts >= s_thresholds[band]) {
        band++;
    }
    if (s_band < 0) {
        s_band = band;
        ESP_LOGI(TAG, "Battery %d mV: %d section(s)", (int)(s_volts * 1000), s_band + 1);
    } else if (band > s_band && s_volts >= s_thresholds[band - 1] + BATTERY_HYSTERESIS_V) {
        s_band = band;
    } else if (band < s_band && s_volts < s_thresholds[s_band - 1] - BATTERY_HYSTERESIS_V) {
        s_band = band;
    }
    *critical = (s_band == 0);
    return s_band + 1;
}
