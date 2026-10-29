#include "wx_awn.h"

#include "cJSON.h"
#include "wx_http.h"
#include "wx_util.h"

#include <stdio.h>
#include <string.h>

static void take_last(ov_sample_t *o, cJSON *d)
{
    wx_take_dir(o, d, "winddir");
    wx_take(o, d, "windspeedmph", OV_MEAS_WIND_SPEED, &o->wind_mms, wx_conv_wind_mph);
    wx_take(o, d, "windgustmph", OV_MEAS_WIND_GUST, &o->gust_mms, wx_conv_wind_mph);
    wx_take(o, d, "tempf", OV_MEAS_TEMP_OUT, &o->temp_out_mc, wx_conv_temp_f);
    wx_take(o, d, "tempinf", OV_MEAS_TEMP_IN, &o->temp_in_mc, wx_conv_temp_f);
    wx_take(o, d, "humidity", OV_MEAS_HUM_OUT, &o->hum_out_pm, wx_conv_hum);
    wx_take(o, d, "humidityin", OV_MEAS_HUM_IN, &o->hum_in_pm, wx_conv_hum);
    wx_take(o, d, "feelsLike", OV_MEAS_FEELS, &o->feels_mc, wx_conv_temp_f);
    wx_take(o, d, "dewPoint", OV_MEAS_DEW, &o->dew_mc, wx_conv_temp_f);
    wx_take(o, d, "baromrelin", OV_MEAS_PRESS_REL, &o->press_rel_pa, wx_conv_press_inhg);
    wx_take(o, d, "baromabsin", OV_MEAS_PRESS_ABS, &o->press_abs_pa, wx_conv_press_inhg);
    wx_take(o, d, "hourlyrainin", OV_MEAS_RAIN_RATE, &o->rain_rate_umh, wx_conv_rain_in);
    wx_take(o, d, "dailyrainin", OV_MEAS_RAIN_DAY, &o->rain_day_um, wx_conv_rain_in);
    wx_take(o, d, "weeklyrainin", OV_MEAS_RAIN_WEEK, &o->rain_week_um, wx_conv_rain_in);
    wx_take(o, d, "monthlyrainin", OV_MEAS_RAIN_MONTH, &o->rain_month_um, wx_conv_rain_in);
    wx_take(o, d, "uv", OV_MEAS_UV, &o->uv_x10, wx_conv_x10);
    wx_take(o, d, "solarradiation", OV_MEAS_SOLAR, &o->solar_wm2, wx_conv_wm2);
    wx_take(o, d, "pm25", OV_MEAS_PM25, &o->pm25_x10, wx_conv_x10);
    wx_take(o, d, "pm10_in_aqin", OV_MEAS_PM10, &o->pm10_x10, wx_conv_x10);
    wx_take(o, d, "pm10", OV_MEAS_PM10, &o->pm10_x10, wx_conv_x10);
    wx_take(o, d, "co2", OV_MEAS_CO2, &o->co2_ppm, wx_conv_count);
    wx_take(o, d, "aqi_pm25_aqin", OV_MEAS_AQI, &o->aqi, wx_conv_count);
    wx_take(o, d, "aqi_pm25_in", OV_MEAS_AQI, &o->aqi, wx_conv_count);
    wx_take(o, d, "lightning_day", OV_MEAS_LTNG_DAY, &o->ltng_day, wx_conv_count);
    wx_take(o, d, "lightning_distance", OV_MEAS_LTNG_DIST, &o->ltng_dist_m, wx_conv_mi);
    wx_take(o, d, "soilhum1", OV_MEAS_SOIL_M, &o->soil_m, wx_conv_hum);
    wx_take(o, d, "soiltemp1f", OV_MEAS_SOIL_T, &o->soil_t_mc, wx_conv_temp_f);
    wx_take(o, d, "leafwetness1", OV_MEAS_LEAF, &o->leaf, wx_conv_hum);
}

static int parse(const char *json, ov_sample_t *out, const char **why)
{
    cJSON *root;
    cJSON *last = NULL;

    if (why)
        *why = "malformed";
    if (!json || !out)
        return -1;
    memset(out, 0, sizeof *out);
    root = cJSON_Parse(json);
    if (!root) {
        if (why)
            *why = "malformed json";
        return -1;
    }
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        if (why)
            *why = "ambient api";
        return -1;
    }
    for (cJSON *dev = root->child; dev; dev = dev->next) {
        cJSON *d = cJSON_GetObjectItemCaseSensitive(dev, "lastData");
        if (cJSON_IsObject(d)) {
            last = d;
            break;
        }
    }
    if (!last) {
        cJSON_Delete(root);
        if (why)
            *why = "no weather data";
        return -1;
    }
    take_last(out, last);
    cJSON_Delete(root);
    wx_finish(out, why);
    return out->ok ? 0 : -1;
}

esp_err_t wx_awn_fetch(const wx_cfg_t *cfg, ov_sample_t *out,
                       int timeout_ms, const char **why)
{
    char url[320];

    if (!cfg || !cfg->app_key[0] || !cfg->api_key[0]) {
        if (why)
            *why = "station address";
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(url, sizeof url,
                 "https://api.ambientweather.net/v1/devices?applicationKey=%s&apiKey=%s",
                 cfg->app_key, cfg->api_key) >= (int)sizeof url) {
        if (why)
            *why = "station address";
        return ESP_ERR_INVALID_ARG;
    }
    return wx_fetch_url(url, NULL, NULL, timeout_ms, parse, out, why, "ambient");
}
