#include "wx_wl.h"

#include "cJSON.h"
#include "wx_http.h"
#include "wx_util.h"

#include <stdio.h>
#include <string.h>

/* Local Live rain is counts. rain_size: 1 = 0.01 in, 2 = 0.2 mm,
 * 3 = 0.1 mm, 4 = 0.001 in. Cloud v2 values are already inches.
 */
static int counts_to_um(int32_t milli_counts, int rain_size, int32_t *um)
{
    double counts, mm;
    if (!um || milli_counts < 0)
        return -1;
    counts = (double)milli_counts / 1000.0;
    if (rain_size == 1)
        mm = counts * 0.254;
    else if (rain_size == 2)
        mm = counts * 0.2;
    else if (rain_size == 3)
        mm = counts * 0.1;
    else if (rain_size == 4)
        mm = counts * 0.0254;
    else
        return -1;
    *um = (int32_t)(mm * 1000.0 + 0.5);
    return 0;
}

static int is_iss(cJSON *rec)
{
    if (!cJSON_IsObject(rec))
        return 0;
    if (cJSON_GetObjectItemCaseSensitive(rec, "wind_speed_last"))
        return 1;
    if (cJSON_GetObjectItemCaseSensitive(rec, "temp") &&
        !cJSON_GetObjectItemCaseSensitive(rec, "pm_2p5") &&
        !cJSON_GetObjectItemCaseSensitive(rec, "moist_soil_1") &&
        !cJSON_GetObjectItemCaseSensitive(rec, "temp_in"))
        return 1;
    return 0;
}

static void take_rain(ov_sample_t *o, cJSON *rec, const char *key, int meas,
                      int32_t *slot, int inches, int rain_size)
{
    int32_t v;
    int32_t um;
    if (!o || (o->has & OV_HAS(meas)))
        return;
    if (wx_num(rec, key, &v) != 0)
        return;
    if (inches) {
        if (wx_conv_rain_in(v, &um) != 0)
            return;
    } else if (counts_to_um(v, rain_size, &um) != 0) {
        return;
    }
    *slot = um;
    o->has |= OV_HAS(meas);
}

static void take_iss(ov_sample_t *o, cJSON *rec, int inches)
{
    int32_t rs = 0;
    int rain_size = 0;
    int before;

    if (wx_num(rec, "rain_size", &rs) == 0)
        rain_size = (int)((rs + 500) / 1000);
    wx_take(o, rec, "temp", OV_MEAS_TEMP_OUT, &o->temp_out_mc, wx_conv_temp_f);
    wx_take(o, rec, "hum", OV_MEAS_HUM_OUT, &o->hum_out_pm, wx_conv_hum);
    wx_take(o, rec, "dew_point", OV_MEAS_DEW, &o->dew_mc, wx_conv_temp_f);
    before = (int)o->has;
    wx_take(o, rec, "thsw_index", OV_MEAS_FEELS, &o->feels_mc, wx_conv_temp_f);
    if (!((o->has ^ (uint32_t)before) & OV_HAS(OV_MEAS_FEELS)))
        wx_take(o, rec, "heat_index", OV_MEAS_FEELS, &o->feels_mc, wx_conv_temp_f);
    if (!(o->has & OV_HAS(OV_MEAS_FEELS)))
        wx_take(o, rec, "wind_chill", OV_MEAS_FEELS, &o->feels_mc, wx_conv_temp_f);
    wx_take(o, rec, "wind_speed_last", OV_MEAS_WIND_SPEED, &o->wind_mms, wx_conv_wind_mph);
    wx_take(o, rec, "wind_speed_hi_last_10_min", OV_MEAS_WIND_GUST, &o->gust_mms, wx_conv_wind_mph);
    wx_take_dir(o, rec, "wind_dir_last");
    wx_take(o, rec, "solar_rad", OV_MEAS_SOLAR, &o->solar_wm2, wx_conv_wm2);
    wx_take(o, rec, "uv_index", OV_MEAS_UV, &o->uv_x10, wx_conv_x10);
    take_rain(o, rec, "rain_rate_last", OV_MEAS_RAIN_RATE, &o->rain_rate_umh, inches, rain_size);
    take_rain(o, rec, "rainfall_last_60_min", OV_MEAS_RAIN_HOUR, &o->rain_hour_um, inches, rain_size);
    take_rain(o, rec, "rainfall_daily", OV_MEAS_RAIN_DAY, &o->rain_day_um, inches, rain_size);
    take_rain(o, rec, "rainfall_monthly", OV_MEAS_RAIN_MONTH, &o->rain_month_um, inches, rain_size);
}

static void take_rest(ov_sample_t *o, cJSON *rec)
{
    uint32_t before;

    wx_take(o, rec, "bar_sea_level", OV_MEAS_PRESS_REL, &o->press_rel_pa, wx_conv_press_inhg);
    wx_take(o, rec, "bar_absolute", OV_MEAS_PRESS_ABS, &o->press_abs_pa, wx_conv_press_inhg);
    wx_take(o, rec, "temp_in", OV_MEAS_TEMP_IN, &o->temp_in_mc, wx_conv_temp_f);
    wx_take(o, rec, "hum_in", OV_MEAS_HUM_IN, &o->hum_in_pm, wx_conv_hum);
    before = o->has;
    wx_take(o, rec, "moist_soil_1", OV_MEAS_SOIL_M, &o->soil_m, wx_conv_cb);
    if (!(before & OV_HAS(OV_MEAS_SOIL_M)) && (o->has & OV_HAS(OV_MEAS_SOIL_M)))
        o->soil_cb = 1;
    wx_take(o, rec, "temp_1", OV_MEAS_SOIL_T, &o->soil_t_mc, wx_conv_temp_f);
    before = o->has;
    wx_take(o, rec, "wet_leaf_1", OV_MEAS_LEAF, &o->leaf, wx_conv_count);
    if (!(before & OV_HAS(OV_MEAS_LEAF)) && (o->has & OV_HAS(OV_MEAS_LEAF)))
        o->leaf_raw = 1;
    wx_take(o, rec, "pm_2p5", OV_MEAS_PM25, &o->pm25_x10, wx_conv_x10);
    wx_take(o, rec, "pm_10", OV_MEAS_PM10, &o->pm10_x10, wx_conv_x10);
    if ((o->has & OV_HAS(OV_MEAS_PM25)) && !(o->has & OV_HAS(OV_MEAS_TEMP_IN)))
        wx_take(o, rec, "temp", OV_MEAS_TEMP_IN, &o->temp_in_mc, wx_conv_temp_f);
    if ((o->has & OV_HAS(OV_MEAS_PM25)) && !(o->has & OV_HAS(OV_MEAS_HUM_IN)))
        wx_take(o, rec, "hum", OV_MEAS_HUM_IN, &o->hum_in_pm, wx_conv_hum);
}

static void one_rec(ov_sample_t *o, cJSON *rec, int inches, int want_iss)
{
    if (!cJSON_IsObject(rec) || is_iss(rec) != want_iss)
        return;
    if (want_iss)
        take_iss(o, rec, inches);
    else
        take_rest(o, rec);
}

static void walk(ov_sample_t *o, cJSON *arr, int inches, int want_iss)
{
    if (!cJSON_IsArray(arr))
        return;
    for (cJSON *it = arr->child; it; it = it->next)
        one_rec(o, it, inches, want_iss);
}

static void walk_sensors(ov_sample_t *o, cJSON *sensors, int inches, int want_iss)
{
    if (!cJSON_IsArray(sensors))
        return;
    for (cJSON *s = sensors->child; s; s = s->next) {
        cJSON *block = cJSON_GetObjectItemCaseSensitive(s, "data");
        cJSON *rec = cJSON_IsArray(block) ? cJSON_GetArrayItem(block, 0) : block;
        one_rec(o, rec, inches, want_iss);
    }
}

static int parse_mode(const char *json, ov_sample_t *out, const char **why, int inches)
{
    cJSON *root;
    cJSON *err;
    cJSON *data;
    cJSON *conds;
    cJSON *sensors;

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
    err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (err && !cJSON_IsNull(err)) {
        cJSON_Delete(root);
        if (why)
            *why = "weatherlink api";
        return -1;
    }
    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    conds = cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "conditions") : NULL;
    sensors = cJSON_GetObjectItemCaseSensitive(root, "sensors");
    if (cJSON_IsArray(conds)) {
        walk(out, conds, inches, 1);
        walk(out, conds, inches, 0);
    } else if (cJSON_IsArray(sensors)) {
        walk_sensors(out, sensors, inches, 1);
        walk_sensors(out, sensors, inches, 0);
    } else {
        cJSON_Delete(root);
        if (why)
            *why = "weatherlink api";
        return -1;
    }
    cJSON_Delete(root);
    wx_finish(out, why);
    return out->ok ? 0 : -1;
}

static int parse_counts(const char *json, ov_sample_t *out, const char **why)
{
    return parse_mode(json, out, why, 0);
}

static int parse_inches(const char *json, ov_sample_t *out, const char **why)
{
    return parse_mode(json, out, why, 1);
}

esp_err_t wx_wl_fetch(const wx_cfg_t *cfg, int sub, ov_sample_t *out,
                      int timeout_ms, const char **why)
{
    char url[384];

    if (!cfg) {
        if (why)
            *why = "station address";
        return ESP_ERR_INVALID_ARG;
    }
    if (sub == OV_SUB_LOCAL) {
        if (!cfg->host[0] || cfg->port < 1) {
            if (why)
                *why = "station address";
            return ESP_ERR_INVALID_ARG;
        }
        if (snprintf(url, sizeof url, "http://%s:%u/v1/current_conditions",
                     cfg->host, (unsigned)cfg->port) >= (int)sizeof url) {
            if (why)
                *why = "station address";
            return ESP_ERR_INVALID_ARG;
        }
        return wx_fetch_url(url, NULL, NULL, timeout_ms, parse_counts, out, why, "weatherlink");
    }
    if (!cfg->api_key[0] || !cfg->api_secret[0] || !cfg->station_id[0]) {
        if (why)
            *why = "station address";
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(url, sizeof url, "https://api.weatherlink.com/v2/current/%s?api-key=%s",
                 cfg->station_id, cfg->api_key) >= (int)sizeof url) {
        if (why)
            *why = "station address";
        return ESP_ERR_INVALID_ARG;
    }
    return wx_fetch_url(url, "X-Api-Secret", cfg->api_secret, timeout_ms,
                        parse_inches, out, why, "weatherlink");
}
