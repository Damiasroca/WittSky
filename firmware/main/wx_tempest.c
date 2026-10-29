#include "wx_tempest.h"

#include "cJSON.h"
#include "wx_http.h"
#include "wx_util.h"

#include <stdio.h>
#include <string.h>

static int arr_at(cJSON *arr, int idx, double *out)
{
    cJSON *it = cJSON_GetArrayItem(arr, idx);
    if (!it || cJSON_IsNull(it) || !cJSON_IsNumber(it))
        return -1;
    *out = it->valuedouble;
    return 0;
}

static void take_idx(ov_sample_t *o, cJSON *arr, int idx, int meas, int32_t *slot,
                     int (*conv)(int32_t m, int32_t *out))
{
    double v;
    int32_t x;
    if (!o || (o->has & OV_HAS(meas)))
        return;
    if (arr_at(arr, idx, &v) != 0 || !conv || conv(wx_milli(v), &x) != 0)
        return;
    *slot = x;
    o->has |= OV_HAS(meas);
}

static void take_idx_dir(ov_sample_t *o, cJSON *arr, int idx)
{
    double v;
    int32_t d;
    if (!o || (o->has & OV_HAS(OV_MEAS_WIND_DIR)))
        return;
    if (arr_at(arr, idx, &v) != 0 || wx_conv_dir(wx_milli(v), &d) != 0)
        return;
    o->dir_deg = (int)d;
    o->has |= OV_HAS(OV_MEAS_WIND_DIR);
}

static void rain_rate(ov_sample_t *o, double rain_mm, double interval_min)
{
    int32_t um;
    if (o->has & OV_HAS(OV_MEAS_RAIN_RATE))
        return;
    if (interval_min < 1)
        interval_min = 1;
    if (wx_conv_rain_mm(wx_milli(rain_mm * 60.0 / interval_min), &um) != 0)
        return;
    o->rain_rate_umh = um;
    o->has |= OV_HAS(OV_MEAS_RAIN_RATE);
}

static void take_st(ov_sample_t *o, cJSON *row)
{
    double rain = 0, final_rain = 0, interval = 0, daily = 0, daily_final = 0;
    int n = cJSON_GetArraySize(row);
    int have_rain, have_final, have_iv, have_day, have_day_final;

    take_idx(o, row, 2, OV_MEAS_WIND_SPEED, &o->wind_mms, wx_conv_wind_ms);
    take_idx(o, row, 3, OV_MEAS_WIND_GUST, &o->gust_mms, wx_conv_wind_ms);
    take_idx_dir(o, row, 4);
    take_idx(o, row, 6, OV_MEAS_PRESS_ABS, &o->press_abs_pa, wx_conv_press_hpa);
    take_idx(o, row, 7, OV_MEAS_TEMP_OUT, &o->temp_out_mc, wx_conv_temp_c);
    take_idx(o, row, 8, OV_MEAS_HUM_OUT, &o->hum_out_pm, wx_conv_hum);
    take_idx(o, row, 10, OV_MEAS_UV, &o->uv_x10, wx_conv_x10);
    take_idx(o, row, 11, OV_MEAS_SOLAR, &o->solar_wm2, wx_conv_wm2);
    have_final = n > 19 && arr_at(row, 19, &final_rain) == 0;
    have_rain = arr_at(row, 12, &rain) == 0;
    have_iv = arr_at(row, 17, &interval) == 0;
    if (have_iv && (have_final || have_rain))
        rain_rate(o, have_final ? final_rain : rain, interval);
    have_day_final = n > 20 && arr_at(row, 20, &daily_final) == 0;
    have_day = arr_at(row, 18, &daily) == 0;
    if (have_day_final)
        take_idx(o, row, 20, OV_MEAS_RAIN_DAY, &o->rain_day_um, wx_conv_rain_mm);
    else if (have_day)
        take_idx(o, row, 18, OV_MEAS_RAIN_DAY, &o->rain_day_um, wx_conv_rain_mm);
    (void)daily;
    (void)daily_final;
}

static void take_sky(ov_sample_t *o, cJSON *row)
{
    double rain = 0, final_rain = 0, interval = 0;
    int n = cJSON_GetArraySize(row);
    int have_rain, have_final, have_iv;

    /* obs_sky: [time, illuminance, uv, rain, wind_lull, wind_avg, wind_gust,
     * wind_dir, battery, report_interval, solar, local_day_rain, precip_type,
     * wind_sample_interval, rain_final, local_day_rain_final].
     */
    take_idx(o, row, 2, OV_MEAS_UV, &o->uv_x10, wx_conv_x10);
    take_idx(o, row, 5, OV_MEAS_WIND_SPEED, &o->wind_mms, wx_conv_wind_ms);
    take_idx(o, row, 6, OV_MEAS_WIND_GUST, &o->gust_mms, wx_conv_wind_ms);
    take_idx_dir(o, row, 7);
    take_idx(o, row, 10, OV_MEAS_SOLAR, &o->solar_wm2, wx_conv_wm2);
    have_final = n > 14 && arr_at(row, 14, &final_rain) == 0;
    have_rain = arr_at(row, 3, &rain) == 0;
    have_iv = arr_at(row, 9, &interval) == 0;
    if (have_iv && (have_final || have_rain))
        rain_rate(o, have_final ? final_rain : rain, interval);
    if (n > 15 && !cJSON_IsNull(cJSON_GetArrayItem(row, 15)))
        take_idx(o, row, 15, OV_MEAS_RAIN_DAY, &o->rain_day_um, wx_conv_rain_mm);
    else
        take_idx(o, row, 11, OV_MEAS_RAIN_DAY, &o->rain_day_um, wx_conv_rain_mm);
}

static void take_air(ov_sample_t *o, cJSON *row)
{
    take_idx(o, row, 1, OV_MEAS_PRESS_ABS, &o->press_abs_pa, wx_conv_press_hpa);
    take_idx(o, row, 2, OV_MEAS_TEMP_OUT, &o->temp_out_mc, wx_conv_temp_c);
    take_idx(o, row, 3, OV_MEAS_HUM_OUT, &o->hum_out_pm, wx_conv_hum);
}

static void take_named(ov_sample_t *o, cJSON *rec)
{
    wx_take(o, rec, "air_temperature", OV_MEAS_TEMP_OUT, &o->temp_out_mc, wx_conv_temp_c);
    wx_take(o, rec, "relative_humidity", OV_MEAS_HUM_OUT, &o->hum_out_pm, wx_conv_hum);
    wx_take(o, rec, "wind_avg", OV_MEAS_WIND_SPEED, &o->wind_mms, wx_conv_wind_ms);
    wx_take(o, rec, "wind_gust", OV_MEAS_WIND_GUST, &o->gust_mms, wx_conv_wind_ms);
    wx_take_dir(o, rec, "wind_direction");
    wx_take(o, rec, "station_pressure", OV_MEAS_PRESS_ABS, &o->press_abs_pa, wx_conv_press_hpa);
    wx_take(o, rec, "sea_level_pressure", OV_MEAS_PRESS_REL, &o->press_rel_pa, wx_conv_press_hpa);
    wx_take(o, rec, "solar_radiation", OV_MEAS_SOLAR, &o->solar_wm2, wx_conv_wm2);
    wx_take(o, rec, "uv", OV_MEAS_UV, &o->uv_x10, wx_conv_x10);
    wx_take(o, rec, "feels_like", OV_MEAS_FEELS, &o->feels_mc, wx_conv_temp_c);
    wx_take(o, rec, "dew_point", OV_MEAS_DEW, &o->dew_mc, wx_conv_temp_c);
    wx_take(o, rec, "precip_accum_local_day", OV_MEAS_RAIN_DAY, &o->rain_day_um, wx_conv_rain_mm);
    wx_take(o, rec, "precip_accum_last_1hr", OV_MEAS_RAIN_HOUR, &o->rain_hour_um, wx_conv_rain_mm);
    wx_take(o, rec, "lightning_strike_last_distance", OV_MEAS_LTNG_DIST, &o->ltng_dist_m, wx_conv_km);
}

static int type_is(cJSON *dev, const char *name)
{
    cJSON *t = cJSON_GetObjectItemCaseSensitive(dev, "type");
    return cJSON_IsString(t) && t->valuestring && strcmp(t->valuestring, name) == 0;
}

static cJSON *row0(cJSON *dev)
{
    cJSON *obs = cJSON_GetObjectItemCaseSensitive(dev, "obs");
    cJSON *row;
    if (!cJSON_IsArray(obs))
        return NULL;
    row = cJSON_GetArrayItem(obs, 0);
    return cJSON_IsArray(row) ? row : NULL;
}

static void take_summary(ov_sample_t *o, cJSON *summary)
{
    if (!cJSON_IsObject(summary))
        return;
    if (cJSON_GetObjectItemCaseSensitive(summary, "strike_last_dist"))
        o->has &= ~OV_HAS(OV_MEAS_LTNG_DIST);
    wx_take(o, summary, "strike_last_dist", OV_MEAS_LTNG_DIST, &o->ltng_dist_m, wx_conv_km);
    if (cJSON_GetObjectItemCaseSensitive(summary, "feels_like"))
        o->has &= ~OV_HAS(OV_MEAS_FEELS);
    wx_take(o, summary, "feels_like", OV_MEAS_FEELS, &o->feels_mc, wx_conv_temp_c);
    if (cJSON_GetObjectItemCaseSensitive(summary, "precip_total_1h"))
        o->has &= ~OV_HAS(OV_MEAS_RAIN_HOUR);
    wx_take(o, summary, "precip_total_1h", OV_MEAS_RAIN_HOUR, &o->rain_hour_um, wx_conv_rain_mm);
}

static void take_device(ov_sample_t *o, cJSON *dev, int air)
{
    cJSON *row = row0(dev);
    cJSON *named;

    if (air) {
        if (type_is(dev, "obs_air") && row)
            take_air(o, row);
        return;
    }
    if (row && (type_is(dev, "obs_st") || type_is(dev, "obs_st_rapid")))
        take_st(o, row);
    else if (row && type_is(dev, "obs_sky"))
        take_sky(o, row);
    else if (!row) {
        named = cJSON_GetObjectItemCaseSensitive(dev, "obs");
        if (cJSON_IsArray(named))
            named = cJSON_GetArrayItem(named, 0);
        if (cJSON_IsObject(named))
            take_named(o, named);
        else if (cJSON_GetObjectItemCaseSensitive(dev, "air_temperature"))
            take_named(o, dev);
    }
    take_summary(o, cJSON_GetObjectItemCaseSensitive(dev, "summary"));
}

static int parse(const char *json, ov_sample_t *out, const char **why)
{
    cJSON *root;
    cJSON *status;
    cJSON *code;
    cJSON *obs;

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
    status = cJSON_GetObjectItemCaseSensitive(root, "status");
    code = cJSON_IsObject(status) ? cJSON_GetObjectItemCaseSensitive(status, "status_code") : NULL;
    if (cJSON_IsNumber(code) && code->valueint != 0) {
        cJSON_Delete(root);
        if (why)
            *why = "tempest api";
        return -1;
    }
    obs = cJSON_GetObjectItemCaseSensitive(root, "obs");
    if (!cJSON_IsArray(obs)) {
        cJSON_Delete(root);
        if (why)
            *why = "tempest api";
        return -1;
    }
    for (cJSON *dev = obs->child; dev; dev = dev->next)
        take_device(out, dev, 0);
    for (cJSON *dev = obs->child; dev; dev = dev->next)
        take_device(out, dev, 1);
    cJSON_Delete(root);
    wx_finish(out, why);
    return out->ok ? 0 : -1;
}

esp_err_t wx_tempest_fetch(const wx_cfg_t *cfg, ov_sample_t *out,
                           int timeout_ms, const char **why)
{
    char url[320];

    if (!cfg || !cfg->token[0] || !cfg->station_id[0]) {
        if (why)
            *why = "station address";
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(url, sizeof url,
                 "https://swd.weatherflow.com/swd/rest/observations/station/%s?token=%s",
                 cfg->station_id, cfg->token) >= (int)sizeof url) {
        if (why)
            *why = "station address";
        return ESP_ERR_INVALID_ARG;
    }
    return wx_fetch_url(url, NULL, NULL, timeout_ms, parse, out, why, "tempest");
}
