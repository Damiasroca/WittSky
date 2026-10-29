#include "wx_util.h"

#include "overlay_units.h"

#include <stdint.h>
#include <string.h>

int32_t wx_milli(double v)
{
    double scaled = v * 1000.0;
    if (scaled >= 2147483647.0)
        return INT32_MAX;
    if (scaled <= -2147483648.0)
        return INT32_MIN;
    if (scaled >= 0)
        return (int32_t)(scaled + 0.5);
    return (int32_t)(scaled - 0.5);
}

int wx_num(cJSON *obj, const char *key, int32_t *out)
{
    cJSON *it;
    char unit[16];

    if (!obj || !out)
        return -1;
    it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it || cJSON_IsNull(it))
        return -1;
    if (cJSON_IsNumber(it)) {
        *out = wx_milli(it->valuedouble);
        return 0;
    }
    if (cJSON_IsString(it) && it->valuestring)
        return ov_parse_milli(it->valuestring, out, unit, sizeof unit);
    return -1;
}

int wx_conv_temp_f(int32_t m, int32_t *out)
{
    if (!out)
        return -1;
    return ov_temp_to_mc(m, "f", out);
}

int wx_conv_temp_c(int32_t m, int32_t *out)
{
    if (!out)
        return -1;
    return ov_temp_to_mc(m, "c", out);
}

int wx_conv_wind_mph(int32_t m, int32_t *out)
{
    if (!out || m < 0)
        return -1;
    return ov_wind_to_mms(m, "mph", out);
}

int wx_conv_wind_ms(int32_t m, int32_t *out)
{
    if (!out || m < 0)
        return -1;
    return ov_wind_to_mms(m, "m/s", out);
}

int wx_conv_wind_kmh(int32_t m, int32_t *out)
{
    if (!out || m < 0)
        return -1;
    return ov_wind_to_mms(m, "km/h", out);
}

int wx_conv_press_inhg(int32_t m, int32_t *out)
{
    if (!out || m < 0)
        return -1;
    return ov_press_to_pa(m, "inhg", out);
}

int wx_conv_press_hpa(int32_t m, int32_t *out)
{
    if (!out || m < 0)
        return -1;
    return ov_press_to_pa(m, "hpa", out);
}

int wx_conv_rain_in(int32_t m, int32_t *out)
{
    if (!out || m < 0)
        return -1;
    return ov_rain_to_um(m, "in", out);
}

int wx_conv_rain_mm(int32_t m, int32_t *out)
{
    if (!out || m < 0)
        return -1;
    return ov_rain_to_um(m, "mm", out);
}

int wx_conv_hum(int32_t m, int32_t *out)
{
    if (!out || m < 0)
        return -1;
    return ov_hum_to_pm(m, "%", out);
}

int wx_conv_x10(int32_t m, int32_t *out)
{
    if (!out || m < 0)
        return -1;
    /* milli -> tenths: divide by 100 with half-up rounding. */
    *out = (m + 50) / 100;
    return 0;
}

int wx_conv_wm2(int32_t m, int32_t *out)
{
    if (!out || m < 0 || m > 20000000)
        return -1;
    *out = (m + 500) / 1000;
    return 0;
}

int wx_conv_count(int32_t m, int32_t *out)
{
    if (!out || m < 0)
        return -1;
    *out = (m + 500) / 1000;
    return 0;
}

int wx_conv_km(int32_t m, int32_t *out)
{
    if (!out || m < 0)
        return -1;
    return ov_dist_to_m(m, "km", out);
}

int wx_conv_mi(int32_t m, int32_t *out)
{
    if (!out || m < 0)
        return -1;
    return ov_dist_to_m(m, "mi", out);
}

int wx_conv_cb(int32_t m, int32_t *out)
{
    if (!out || m < 0)
        return -1;
    *out = m;
    return 0;
}

int wx_conv_dir(int32_t m, int32_t *out)
{
    int d;
    if (!out || m < 0 || m > 360000)
        return -1;
    d = (m + 500) / 1000;
    if (d == 360)
        d = 0;
    if (d < 0 || d > 359)
        return -1;
    *out = d;
    return 0;
}

void wx_take(ov_sample_t *o, cJSON *obj, const char *key, int meas,
             int32_t *slot, int (*conv)(int32_t m, int32_t *out))
{
    int32_t v, x;
    if (!o || !slot || (o->has & OV_HAS(meas)))
        return;
    if (wx_num(obj, key, &v) != 0)
        return;
    if (!conv || conv(v, &x) != 0)
        return;
    *slot = x;
    o->has |= OV_HAS(meas);
}

void wx_take_dir(ov_sample_t *o, cJSON *obj, const char *key)
{
    int32_t v, d;
    if (!o || (o->has & OV_HAS(OV_MEAS_WIND_DIR)))
        return;
    if (wx_num(obj, key, &v) != 0)
        return;
    if (wx_conv_dir(v, &d) != 0)
        return;
    o->dir_deg = (int)d;
    o->has |= OV_HAS(OV_MEAS_WIND_DIR);
}

void wx_finish(ov_sample_t *o, const char **why)
{
    if (!o)
        return;
    if ((o->has & OV_HAS(OV_MEAS_RAIN_RATE)) && o->rain_rate_umh > 0)
        o->raining = true;
    if (!o->has) {
        o->ok = false;
        if (why)
            *why = "no weather data";
        return;
    }
    o->ok = true;
    if (why)
        *why = NULL;
}
