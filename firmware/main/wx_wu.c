#include "wx_wu.h"

#include "cJSON.h"
#include "wx_http.h"
#include "wx_util.h"

#include <stdio.h>
#include <string.h>

static void take_obs(ov_sample_t *o, cJSON *rec, cJSON *m)
{
    wx_take(o, m, "temp", OV_MEAS_TEMP_OUT, &o->temp_out_mc, wx_conv_temp_c);
    wx_take(o, m, "dewpt", OV_MEAS_DEW, &o->dew_mc, wx_conv_temp_c);
    wx_take(o, m, "windSpeed", OV_MEAS_WIND_SPEED, &o->wind_mms, wx_conv_wind_kmh);
    wx_take(o, m, "windGust", OV_MEAS_WIND_GUST, &o->gust_mms, wx_conv_wind_kmh);
    wx_take(o, m, "pressure", OV_MEAS_PRESS_REL, &o->press_rel_pa, wx_conv_press_hpa);
    wx_take(o, m, "precipRate", OV_MEAS_RAIN_RATE, &o->rain_rate_umh, wx_conv_rain_mm);
    wx_take(o, m, "precipTotal", OV_MEAS_RAIN_DAY, &o->rain_day_um, wx_conv_rain_mm);
    wx_take(o, rec, "humidity", OV_MEAS_HUM_OUT, &o->hum_out_pm, wx_conv_hum);
    wx_take(o, rec, "uv", OV_MEAS_UV, &o->uv_x10, wx_conv_x10);
    wx_take(o, rec, "solarRadiation", OV_MEAS_SOLAR, &o->solar_wm2, wx_conv_wm2);
    wx_take_dir(o, rec, "winddir");
    if ((o->has & OV_HAS(OV_MEAS_TEMP_OUT)) && o->temp_out_mc < 15000)
        wx_take(o, m, "windChill", OV_MEAS_FEELS, &o->feels_mc, wx_conv_temp_c);
    if (!(o->has & OV_HAS(OV_MEAS_FEELS)))
        wx_take(o, m, "heatIndex", OV_MEAS_FEELS, &o->feels_mc, wx_conv_temp_c);
    if (!(o->has & OV_HAS(OV_MEAS_FEELS)))
        wx_take(o, m, "windChill", OV_MEAS_FEELS, &o->feels_mc, wx_conv_temp_c);
}

static int parse(const char *json, ov_sample_t *out, const char **why)
{
    cJSON *root;
    cJSON *obs;
    cJSON *rec;
    cJSON *metric;

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
    obs = cJSON_GetObjectItemCaseSensitive(root, "observations");
    rec = cJSON_IsArray(obs) ? cJSON_GetArrayItem(obs, 0) : NULL;
    metric = cJSON_IsObject(rec) ? cJSON_GetObjectItemCaseSensitive(rec, "metric") : NULL;
    if (!cJSON_IsObject(metric)) {
        cJSON_Delete(root);
        if (why)
            *why = "wunderground api";
        return -1;
    }
    take_obs(out, rec, metric);
    cJSON_Delete(root);
    wx_finish(out, why);
    return out->ok ? 0 : -1;
}

esp_err_t wx_wu_fetch(const wx_cfg_t *cfg, ov_sample_t *out,
                      int timeout_ms, const char **why)
{
    char url[384];

    if (!cfg || !cfg->api_key[0] || !cfg->station_id[0]) {
        if (why)
            *why = "station address";
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(url, sizeof url,
                 "https://api.weather.com/v2/pws/observations/current"
                 "?stationId=%s&format=json&units=m&numericPrecision=decimal&apiKey=%s",
                 cfg->station_id, cfg->api_key) >= (int)sizeof url) {
        if (why)
            *why = "station address";
        return ESP_ERR_INVALID_ARG;
    }
    return wx_fetch_url(url, NULL, NULL, timeout_ms, parse, out, why, "wunderground");
}
