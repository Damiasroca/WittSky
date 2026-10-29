#pragma once

#include "overlay_meas.h"

#include <stdbool.h>
#include <stdint.h>

/* Internal units. Display units are applied only when the text is built.
 * Humidity and leaf wetness (unless leaf_raw) are per-mille: 650 = 65.0%.
 * Soil moisture is per-mille percent, or milli-centibar when soil_cb is set.
 * Pressure is pascals. Rain is micrometres (and um/h for the rate).
 * PM is tenths of a ug/m3. UV is tenths of an index.
 */
typedef struct {
    int32_t wind_mms;
    int32_t gust_mms;
    int dir_deg;
    int32_t temp_out_mc;
    int32_t temp_in_mc;
    int32_t hum_out_pm;
    int32_t hum_in_pm;
    int32_t feels_mc;
    int32_t dew_mc;
    int32_t press_rel_pa;
    int32_t press_abs_pa;
    int32_t rain_rate_umh;
    int32_t rain_hour_um;
    int32_t rain_day_um;
    int32_t rain_week_um;
    int32_t rain_month_um;
    int32_t uv_x10;
    int32_t solar_wm2;
    int32_t ltng_dist_m;
    int32_t ltng_day;
    int32_t pm25_x10;
    int32_t pm10_x10;
    int32_t co2_ppm;
    int32_t aqi;
    int32_t soil_m;
    int32_t soil_t_mc;
    int32_t leaf;
    uint8_t soil_cb;
    uint8_t leaf_raw;
    uint32_t has;
    bool raining;
    bool ok;
    int fetch_ms;
} ov_sample_t;

typedef struct {
    char host[64];
    uint16_t port;
    char app_key[64];
    char api_key[64];
    char api_secret[64];
    char token[128];
    char station_id[48];
    char url[384]; /* legacy Ecowitt cloud URL; used until the next save */
} wx_cfg_t;
