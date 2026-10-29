#include "wx_eco.h"

#include "cJSON.h"
#include "overlay_units.h"

#include <stdio.h>
#include <string.h>

#if !defined(OV_HOST)
#include "wx_http.h"
#endif

/* Half-up division for m >= 0 without the overflow of (m + d / 2). */
static int32_t rdiv(int32_t m, int32_t d)
{
    return m / d + ((m % d) >= d / 2 ? 1 : 0);
}

static void snprintf_num(char *dst, size_t n, double v)
{
    int ip;
    int frac;
    int neg = v < 0;
    if (neg)
        v = -v;
    if (v > 2000000000.0)
        v = 2000000000.0;
    ip = (int)v;
    frac = (int)((v - (double)ip) * 1000.0 + 0.5);
    if (frac >= 1000) {
        ip++;
        frac -= 1000;
    }
    if (neg)
        snprintf(dst, n, "-%d.%03d", ip, frac);
    else
        snprintf(dst, n, "%d.%03d", ip, frac);
}

static int icmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return (unsigned char)ca - (unsigned char)cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static cJSON *find_id(cJSON *arr, const char *id)
{
    if (!cJSON_IsArray(arr))
        return NULL;
    for (cJSON *it = arr->child; it; it = it->next) {
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(it, "id");
        if (cJSON_IsString(jid) && jid->valuestring &&
            icmp(jid->valuestring, id) == 0)
            return it;
    }
    return NULL;
}

static cJSON *ch_obj(cJSON *arr, const char *ch)
{
    if (!cJSON_IsArray(arr))
        return NULL;
    for (cJSON *it = arr->child; it; it = it->next) {
        cJSON *c = cJSON_GetObjectItemCaseSensitive(it, "channel");
        if (cJSON_IsString(c) && c->valuestring && strcmp(c->valuestring, ch) == 0)
            return it;
        if (cJSON_IsNumber(c) && c->valueint == 1 && ch[0] == '1' && ch[1] == 0)
            return it;
    }
    return NULL;
}

static cJSON *first_obj(cJSON *arr)
{
    if (cJSON_IsArray(arr))
        return cJSON_IsObject(arr->child) ? arr->child : NULL;
    if (cJSON_IsObject(arr))
        return arr;
    return NULL;
}

static int item_measure(cJSON *it, int32_t *milli, char *unit, size_t unit_n)
{
    cJSON *val = cJSON_GetObjectItemCaseSensitive(it, "val");
    char tmp[32];
    const char *s = NULL;

    unit[0] = 0;
    if (cJSON_IsString(val) && val->valuestring) {
        s = val->valuestring;
    } else if (cJSON_IsNumber(val)) {
        snprintf_num(tmp, sizeof tmp, val->valuedouble);
        s = tmp;
    } else {
        return -1;
    }
    if (ov_parse_milli(s, milli, unit, unit_n) != 0)
        return -1;
    if (!unit[0]) {
        cJSON *u = cJSON_GetObjectItemCaseSensitive(it, "unit");
        if (cJSON_IsString(u) && u->valuestring) {
            size_t i = 0;
            while (u->valuestring[i] && i + 1 < unit_n) {
                unit[i] = u->valuestring[i];
                i++;
            }
            unit[i] = 0;
        }
    }
    return 0;
}

static int cloud_point(cJSON *obj, int32_t *milli, char *unit, size_t unit_n)
{
    cJSON *val;
    cJSON *u;
    char tmp[32];
    const char *s = NULL;
    size_t i = 0;

    if (!cJSON_IsObject(obj))
        return -1;
    unit[0] = 0;
    val = cJSON_GetObjectItemCaseSensitive(obj, "value");
    if (cJSON_IsString(val) && val->valuestring)
        s = val->valuestring;
    else if (cJSON_IsNumber(val)) {
        snprintf_num(tmp, sizeof tmp, val->valuedouble);
        s = tmp;
    } else {
        return -1;
    }
    if (ov_parse_milli(s, milli, unit, unit_n) != 0)
        return -1;
    u = cJSON_GetObjectItemCaseSensitive(obj, "unit");
    if (cJSON_IsString(u) && u->valuestring) {
        while (u->valuestring[i] && i + 1 < unit_n) {
            unit[i] = u->valuestring[i];
            i++;
        }
        unit[i] = 0;
    }
    return 0;
}

static int read_id(cJSON *arr, const char *id, int32_t *milli, char *unit, size_t n)
{
    cJSON *it = find_id(arr, id);
    if (!it)
        return 0;
    if (item_measure(it, milli, unit, n) != 0)
        return -1;
    return 1;
}

static int field_milli(cJSON *obj, const char *key, int32_t *milli, char *unit, size_t n)
{
    cJSON *it;
    char tmp[32];
    const char *s = NULL;
    cJSON *u;
    size_t i = 0;

    if (!cJSON_IsObject(obj))
        return 0;
    it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it || cJSON_IsNull(it))
        return 0;
    unit[0] = 0;
    if (cJSON_IsString(it) && it->valuestring)
        s = it->valuestring;
    else if (cJSON_IsNumber(it)) {
        snprintf_num(tmp, sizeof tmp, it->valuedouble);
        s = tmp;
    } else {
        return -1;
    }
    if (ov_parse_milli(s, milli, unit, n) != 0)
        return -1;
    if (!unit[0]) {
        u = cJSON_GetObjectItemCaseSensitive(obj, "unit");
        if (cJSON_IsString(u) && u->valuestring) {
            while (u->valuestring[i] && i + 1 < n) {
                unit[i] = u->valuestring[i];
                i++;
            }
            unit[i] = 0;
        }
    }
    return 1;
}

static int cloud_milli(cJSON *parent, const char *key, int32_t *milli, char *unit, size_t n)
{
    cJSON *it;
    if (!cJSON_IsObject(parent))
        return 0;
    it = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!it)
        return 0;
    if (cloud_point(it, milli, unit, n) != 0)
        return -1;
    return 1;
}

static void set_i(ov_sample_t *o, int meas, int32_t *slot, int32_t v)
{
    if (o->has & OV_HAS(meas))
        return;
    *slot = v;
    o->has |= OV_HAS(meas);
}

static int take_temp_milli(ov_sample_t *o, int meas, int32_t *slot, int rc,
                           int32_t milli, const char *unit)
{
    int32_t mc;
    if (rc <= 0)
        return rc;
    if (ov_temp_to_mc(milli, unit, &mc) != 0)
        return -1;
    set_i(o, meas, slot, mc);
    return 1;
}

static int take_wind_milli(ov_sample_t *o, int meas, int32_t *slot, int rc,
                           int32_t milli, const char *unit)
{
    int32_t mms;
    if (rc <= 0)
        return rc;
    if (ov_wind_to_mms(milli, unit, &mms) != 0)
        return -1;
    set_i(o, meas, slot, mms);
    return 1;
}

static int take_hum_milli(ov_sample_t *o, int meas, int32_t *slot, int rc,
                          int32_t milli, const char *unit)
{
    int32_t pm;
    if (rc <= 0)
        return rc;
    if (ov_hum_to_pm(milli, unit, &pm) != 0)
        return -1;
    set_i(o, meas, slot, pm);
    return 1;
}

static int take_press_milli(ov_sample_t *o, int meas, int32_t *slot, int rc,
                            int32_t milli, const char *unit)
{
    int32_t pa;
    if (rc <= 0)
        return rc;
    if (ov_press_to_pa(milli, unit, &pa) != 0)
        return -1;
    set_i(o, meas, slot, pa);
    return 1;
}

static int take_dir_milli(ov_sample_t *o, int rc, int32_t milli)
{
    int deg;
    if (rc <= 0)
        return rc;
    if (o->has & OV_HAS(OV_MEAS_WIND_DIR))
        return 1;
    deg = (int)((milli >= 0 ? milli + 500 : milli - 500) / 1000);
    if (deg < 0 || deg > 360)
        return -1;
    o->dir_deg = deg % 360;
    o->has |= OV_HAS(OV_MEAS_WIND_DIR);
    return 1;
}

static int is_lux(const char *unit)
{
    char u[16];
    size_t i = 0;
    if (!unit)
        return 0;
    while (unit[i] && i + 1 < sizeof u) {
        char c = unit[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        u[i++] = c;
    }
    u[i] = 0;
    return strcmp(u, "lux") == 0;
}

static int take_solar_milli(ov_sample_t *o, int rc, int32_t milli, const char *unit)
{
    if (rc <= 0)
        return rc;
    if (is_lux(unit))
        return 0;
    if (milli < 0)
        return -1;
    set_i(o, OV_MEAS_SOLAR, &o->solar_wm2, rdiv(milli, 1000));
    return 1;
}

static int take_uv_milli(ov_sample_t *o, int rc, int32_t milli)
{
    if (rc <= 0)
        return rc;
    if (milli < 0)
        return -1;
    set_i(o, OV_MEAS_UV, &o->uv_x10, rdiv(milli, 100));
    return 1;
}

static int take_x10_milli(ov_sample_t *o, int meas, int32_t *slot, int rc, int32_t milli)
{
    if (rc <= 0)
        return rc;
    if (milli < 0)
        return -1;
    set_i(o, meas, slot, rdiv(milli, 100));
    return 1;
}

static int take_count_milli(ov_sample_t *o, int meas, int32_t *slot, int rc, int32_t milli)
{
    if (rc <= 0)
        return rc;
    if (milli < 0)
        return -1;
    set_i(o, meas, slot, rdiv(milli, 1000));
    return 1;
}

static int take_dist_milli(ov_sample_t *o, int rc, int32_t milli, const char *unit)
{
    int32_t m;
    if (rc <= 0)
        return rc;
    if (ov_dist_to_m(milli, unit, &m) != 0)
        return -1;
    set_i(o, OV_MEAS_LTNG_DIST, &o->ltng_dist_m, m);
    return 1;
}

static int piezo_on(cJSON *it)
{
    cJSON *val;
    if (!it)
        return 0;
    val = cJSON_GetObjectItemCaseSensitive(it, "val");
    if (!val)
        val = cJSON_GetObjectItemCaseSensitive(it, "value");
    if (cJSON_IsString(val) && val->valuestring && val->valuestring[0] == '1' &&
        val->valuestring[1] == 0)
        return 1;
    if (cJSON_IsNumber(val) && val->valueint == 1)
        return 1;
    return 0;
}

static int cloud_state_on(cJSON *piezo)
{
    cJSON *it;
    if (!cJSON_IsObject(piezo))
        return 0;
    it = cJSON_GetObjectItemCaseSensitive(piezo, "state");
    return piezo_on(it);
}

static void note_rain(ov_sample_t *o)
{
    if ((o->has & OV_HAS(OV_MEAS_RAIN_RATE)) && o->rain_rate_umh > 0)
        o->raining = true;
}

static int take_rain_id(ov_sample_t *o, cJSON *root, const char *id, int meas,
                        int32_t *slot, int strict)
{
    int32_t milli, um_a = 0, um_b = 0;
    char unit[16];
    int ha, hb;
    int32_t chosen = 0;
    int have = 0;

    ha = read_id(cJSON_GetObjectItemCaseSensitive(root, "piezoRain"), id,
                 &milli, unit, sizeof unit);
    if (ha > 0) {
        if (ov_rain_to_um(milli, unit, &um_a) != 0)
            ha = -1;
    }
    hb = read_id(cJSON_GetObjectItemCaseSensitive(root, "rain"), id,
                 &milli, unit, sizeof unit);
    if (hb > 0) {
        if (ov_rain_to_um(milli, unit, &um_b) != 0)
            hb = -1;
    }
    if (strict && (ha < 0 || hb < 0))
        return -1;
    if (ha > 0 && um_a > 0) {
        chosen = um_a;
        have = 1;
    } else if (hb > 0 && um_b > 0) {
        chosen = um_b;
        have = 1;
    } else if (ha > 0) {
        chosen = um_a;
        have = 1;
    } else if (hb > 0) {
        chosen = um_b;
        have = 1;
    }
    if (have)
        set_i(o, meas, slot, chosen);
    return 0;
}

static int take_cloud_rain(ov_sample_t *o, cJSON *data, const char *key, int meas,
                           int32_t *slot, int strict)
{
    int32_t milli, um_a = 0, um_b = 0;
    char unit[16];
    int ha, hb;
    int32_t chosen = 0;
    int have = 0;
    cJSON *piezo = cJSON_GetObjectItemCaseSensitive(data, "rainfall_piezo");
    cJSON *bucket = cJSON_GetObjectItemCaseSensitive(data, "rainfall");

    ha = cloud_milli(piezo, key, &milli, unit, sizeof unit);
    if (ha > 0 && ov_rain_to_um(milli, unit, &um_a) != 0)
        ha = -1;
    hb = cloud_milli(bucket, key, &milli, unit, sizeof unit);
    if (hb > 0 && ov_rain_to_um(milli, unit, &um_b) != 0)
        hb = -1;
    if (strict && (ha < 0 || hb < 0))
        return -1;
    if (ha > 0 && um_a > 0) {
        chosen = um_a;
        have = 1;
    } else if (hb > 0 && um_b > 0) {
        chosen = um_b;
        have = 1;
    } else if (ha > 0) {
        chosen = um_a;
        have = 1;
    } else if (hb > 0) {
        chosen = um_b;
        have = 1;
    }
    if (have)
        set_i(o, meas, slot, chosen);
    return 0;
}

static void take_common(ov_sample_t *o, cJSON *common, const char **why, int *bad)
{
    int32_t milli = 0;
    char unit[16];
    int rc;

    unit[0] = 0;
    rc = read_id(common, "0x02", &milli, unit, sizeof unit);
    if (take_temp_milli(o, OV_MEAS_TEMP_OUT, &o->temp_out_mc, rc, milli, unit) < 0) {
        *why = "outdoor temperature";
        *bad = 1;
        return;
    }
    rc = read_id(common, "0x01", &milli, unit, sizeof unit);
    (void)take_temp_milli(o, OV_MEAS_TEMP_IN, &o->temp_in_mc, rc, milli, unit);
    rc = read_id(common, "0x07", &milli, unit, sizeof unit);
    (void)take_hum_milli(o, OV_MEAS_HUM_OUT, &o->hum_out_pm, rc, milli, unit);
    rc = read_id(common, "0x06", &milli, unit, sizeof unit);
    (void)take_hum_milli(o, OV_MEAS_HUM_IN, &o->hum_in_pm, rc, milli, unit);
    rc = read_id(common, "3", &milli, unit, sizeof unit);
    (void)take_temp_milli(o, OV_MEAS_FEELS, &o->feels_mc, rc, milli, unit);
    rc = read_id(common, "0x05", &milli, unit, sizeof unit);
    (void)take_temp_milli(o, OV_MEAS_FEELS, &o->feels_mc, rc, milli, unit);
    rc = read_id(common, "0x03", &milli, unit, sizeof unit);
    (void)take_temp_milli(o, OV_MEAS_DEW, &o->dew_mc, rc, milli, unit);
    rc = read_id(common, "0x08", &milli, unit, sizeof unit);
    (void)take_press_milli(o, OV_MEAS_PRESS_ABS, &o->press_abs_pa, rc, milli, unit);
    rc = read_id(common, "0x09", &milli, unit, sizeof unit);
    (void)take_press_milli(o, OV_MEAS_PRESS_REL, &o->press_rel_pa, rc, milli, unit);
    rc = read_id(common, "0x0B", &milli, unit, sizeof unit);
    if (take_wind_milli(o, OV_MEAS_WIND_SPEED, &o->wind_mms, rc, milli, unit) < 0) {
        *why = "wind speed";
        *bad = 1;
        return;
    }
    rc = read_id(common, "0x0C", &milli, unit, sizeof unit);
    (void)take_wind_milli(o, OV_MEAS_WIND_GUST, &o->gust_mms, rc, milli, unit);
    rc = read_id(common, "0x0A", &milli, unit, sizeof unit);
    if (take_dir_milli(o, rc, milli) < 0) {
        *why = "wind direction";
        *bad = 1;
        return;
    }
    rc = read_id(common, "0x15", &milli, unit, sizeof unit);
    (void)take_solar_milli(o, rc, milli, unit);
    rc = read_id(common, "0x17", &milli, unit, sizeof unit);
    (void)take_uv_milli(o, rc, milli);
}

static void take_wh25(ov_sample_t *o, cJSON *root)
{
    cJSON *wh = first_obj(cJSON_GetObjectItemCaseSensitive(root, "wh25"));
    int32_t milli = 0;
    char unit[16];
    int rc;
    unit[0] = 0;
    if (!wh)
        return;
    rc = field_milli(wh, "intemp", &milli, unit, sizeof unit);
    (void)take_temp_milli(o, OV_MEAS_TEMP_IN, &o->temp_in_mc, rc, milli, unit);
    rc = field_milli(wh, "inhumi", &milli, unit, sizeof unit);
    (void)take_hum_milli(o, OV_MEAS_HUM_IN, &o->hum_in_pm, rc, milli, unit);
    rc = field_milli(wh, "abs", &milli, unit, sizeof unit);
    (void)take_press_milli(o, OV_MEAS_PRESS_ABS, &o->press_abs_pa, rc, milli, unit);
    rc = field_milli(wh, "rel", &milli, unit, sizeof unit);
    (void)take_press_milli(o, OV_MEAS_PRESS_REL, &o->press_rel_pa, rc, milli, unit);
}

static void take_local_extra(ov_sample_t *o, cJSON *root)
{
    cJSON *lt = first_obj(cJSON_GetObjectItemCaseSensitive(root, "lightning"));
    cJSON *pm = ch_obj(cJSON_GetObjectItemCaseSensitive(root, "ch_pm25"), "1");
    cJSON *co2 = first_obj(cJSON_GetObjectItemCaseSensitive(root, "co2"));
    cJSON *soil = ch_obj(cJSON_GetObjectItemCaseSensitive(root, "ch_soil"), "1");
    cJSON *leaf = ch_obj(cJSON_GetObjectItemCaseSensitive(root, "ch_leaf"), "1");
    cJSON *flag;
    int32_t milli = 0;
    char unit[16];
    int rc;

    unit[0] = 0;
    if (lt) {
        rc = field_milli(lt, "distance", &milli, unit, sizeof unit);
        (void)take_dist_milli(o, rc, milli, unit);
        rc = field_milli(lt, "count", &milli, unit, sizeof unit);
        (void)take_count_milli(o, OV_MEAS_LTNG_DAY, &o->ltng_day, rc, milli);
    }
    if (pm) {
        rc = field_milli(pm, "PM25", &milli, unit, sizeof unit);
        (void)take_x10_milli(o, OV_MEAS_PM25, &o->pm25_x10, rc, milli);
        rc = field_milli(pm, "PM25_RealAQI", &milli, unit, sizeof unit);
        (void)take_count_milli(o, OV_MEAS_AQI, &o->aqi, rc, milli);
    }
    if (co2) {
        rc = field_milli(co2, "PM25", &milli, unit, sizeof unit);
        (void)take_x10_milli(o, OV_MEAS_PM25, &o->pm25_x10, rc, milli);
        rc = field_milli(co2, "PM10", &milli, unit, sizeof unit);
        (void)take_x10_milli(o, OV_MEAS_PM10, &o->pm10_x10, rc, milli);
        rc = field_milli(co2, "CO2", &milli, unit, sizeof unit);
        (void)take_count_milli(o, OV_MEAS_CO2, &o->co2_ppm, rc, milli);
        rc = field_milli(co2, "PM25_RealAQI", &milli, unit, sizeof unit);
        (void)take_count_milli(o, OV_MEAS_AQI, &o->aqi, rc, milli);
    }
    if (soil) {
        rc = field_milli(soil, "humidity", &milli, unit, sizeof unit);
        (void)take_hum_milli(o, OV_MEAS_SOIL_M, &o->soil_m, rc, milli, unit);
        rc = field_milli(soil, "temp", &milli, unit, sizeof unit);
        (void)take_temp_milli(o, OV_MEAS_SOIL_T, &o->soil_t_mc, rc, milli, unit);
    }
    if (leaf) {
        rc = field_milli(leaf, "humidity", &milli, unit, sizeof unit);
        (void)take_hum_milli(o, OV_MEAS_LEAF, &o->leaf, rc, milli, unit);
    }
    flag = find_id(cJSON_GetObjectItemCaseSensitive(root, "piezoRain"), "srain_piezo");
    if (piezo_on(flag))
        o->raining = true;
}

static void take_cloud_group(ov_sample_t *o, cJSON *data)
{
    cJSON *outdoor = cJSON_GetObjectItemCaseSensitive(data, "outdoor");
    cJSON *indoor = cJSON_GetObjectItemCaseSensitive(data, "indoor");
    cJSON *wind = cJSON_GetObjectItemCaseSensitive(data, "wind");
    cJSON *press = cJSON_GetObjectItemCaseSensitive(data, "pressure");
    cJSON *solar = cJSON_GetObjectItemCaseSensitive(data, "solar_and_uvi");
    cJSON *lt = cJSON_GetObjectItemCaseSensitive(data, "lightning");
    cJSON *pm = cJSON_GetObjectItemCaseSensitive(data, "pm25_ch1");
    cJSON *pm10 = cJSON_GetObjectItemCaseSensitive(data, "pm10_aqi_combo");
    cJSON *co2 = cJSON_GetObjectItemCaseSensitive(data, "indoor_co2");
    cJSON *co2b = cJSON_GetObjectItemCaseSensitive(data, "co2_aqi_combo");
    cJSON *soil = cJSON_GetObjectItemCaseSensitive(data, "soil_ch1");
    cJSON *soilt = cJSON_GetObjectItemCaseSensitive(data, "ch_soil_ec_temp_hum1");
    cJSON *leaf = cJSON_GetObjectItemCaseSensitive(data, "leaf_ch1");
    cJSON *aqi = cJSON_GetObjectItemCaseSensitive(data, "pm25_aqi_combo");
    int32_t milli = 0;
    char unit[16];
    int rc;

    unit[0] = 0;
    rc = cloud_milli(outdoor, "temperature", &milli, unit, sizeof unit);
    (void)take_temp_milli(o, OV_MEAS_TEMP_OUT, &o->temp_out_mc, rc, milli, unit);
    rc = cloud_milli(outdoor, "feels_like", &milli, unit, sizeof unit);
    (void)take_temp_milli(o, OV_MEAS_FEELS, &o->feels_mc, rc, milli, unit);
    rc = cloud_milli(outdoor, "dew_point", &milli, unit, sizeof unit);
    (void)take_temp_milli(o, OV_MEAS_DEW, &o->dew_mc, rc, milli, unit);
    rc = cloud_milli(outdoor, "humidity", &milli, unit, sizeof unit);
    (void)take_hum_milli(o, OV_MEAS_HUM_OUT, &o->hum_out_pm, rc, milli, unit);
    rc = cloud_milli(indoor, "temperature", &milli, unit, sizeof unit);
    (void)take_temp_milli(o, OV_MEAS_TEMP_IN, &o->temp_in_mc, rc, milli, unit);
    rc = cloud_milli(indoor, "humidity", &milli, unit, sizeof unit);
    (void)take_hum_milli(o, OV_MEAS_HUM_IN, &o->hum_in_pm, rc, milli, unit);
    rc = cloud_milli(wind, "wind_speed", &milli, unit, sizeof unit);
    (void)take_wind_milli(o, OV_MEAS_WIND_SPEED, &o->wind_mms, rc, milli, unit);
    rc = cloud_milli(wind, "wind_gust", &milli, unit, sizeof unit);
    (void)take_wind_milli(o, OV_MEAS_WIND_GUST, &o->gust_mms, rc, milli, unit);
    rc = cloud_milli(wind, "wind_direction", &milli, unit, sizeof unit);
    (void)take_dir_milli(o, rc, milli);
    rc = cloud_milli(press, "relative", &milli, unit, sizeof unit);
    (void)take_press_milli(o, OV_MEAS_PRESS_REL, &o->press_rel_pa, rc, milli, unit);
    rc = cloud_milli(press, "absolute", &milli, unit, sizeof unit);
    (void)take_press_milli(o, OV_MEAS_PRESS_ABS, &o->press_abs_pa, rc, milli, unit);
    rc = cloud_milli(solar, "solar", &milli, unit, sizeof unit);
    (void)take_solar_milli(o, rc, milli, unit);
    rc = cloud_milli(solar, "uvi", &milli, unit, sizeof unit);
    (void)take_uv_milli(o, rc, milli);
    rc = cloud_milli(lt, "distance", &milli, unit, sizeof unit);
    (void)take_dist_milli(o, rc, milli, unit);
    rc = cloud_milli(lt, "count", &milli, unit, sizeof unit);
    (void)take_count_milli(o, OV_MEAS_LTNG_DAY, &o->ltng_day, rc, milli);
    rc = cloud_milli(pm, "pm25", &milli, unit, sizeof unit);
    (void)take_x10_milli(o, OV_MEAS_PM25, &o->pm25_x10, rc, milli);
    rc = cloud_milli(pm, "real_time_aqi", &milli, unit, sizeof unit);
    (void)take_count_milli(o, OV_MEAS_AQI, &o->aqi, rc, milli);
    rc = cloud_milli(aqi, "real_time_aqi", &milli, unit, sizeof unit);
    (void)take_count_milli(o, OV_MEAS_AQI, &o->aqi, rc, milli);
    rc = cloud_milli(pm10, "pm10", &milli, unit, sizeof unit);
    (void)take_x10_milli(o, OV_MEAS_PM10, &o->pm10_x10, rc, milli);
    rc = cloud_milli(co2, "co2", &milli, unit, sizeof unit);
    (void)take_count_milli(o, OV_MEAS_CO2, &o->co2_ppm, rc, milli);
    rc = cloud_milli(co2b, "co2", &milli, unit, sizeof unit);
    (void)take_count_milli(o, OV_MEAS_CO2, &o->co2_ppm, rc, milli);
    rc = cloud_milli(soil, "soilmoisture", &milli, unit, sizeof unit);
    (void)take_hum_milli(o, OV_MEAS_SOIL_M, &o->soil_m, rc, milli, unit);
    rc = cloud_milli(soilt, "temperature", &milli, unit, sizeof unit);
    (void)take_temp_milli(o, OV_MEAS_SOIL_T, &o->soil_t_mc, rc, milli, unit);
    rc = cloud_milli(leaf, "leaf_wetness", &milli, unit, sizeof unit);
    (void)take_hum_milli(o, OV_MEAS_LEAF, &o->leaf, rc, milli, unit);
}

static int api_rejected(cJSON *root)
{
    cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!code)
        return 0;
    if (cJSON_IsNumber(code))
        return code->valueint != 0;
    if (cJSON_IsString(code) && code->valuestring)
        return strcmp(code->valuestring, "0") != 0;
    return 1;
}

static int parse_cloud(cJSON *data, ov_sample_t *out, const char **why)
{
    int32_t milli = 0;
    char unit[16];
    int rc;

    unit[0] = 0;
    rc = cloud_milli(cJSON_GetObjectItemCaseSensitive(data, "outdoor"),
                     "temperature", &milli, unit, sizeof unit);
    if (rc < 0 || (rc > 0 && take_temp_milli(out, OV_MEAS_TEMP_OUT, &out->temp_out_mc,
                                             rc, milli, unit) < 0)) {
        if (why)
            *why = "outdoor temperature";
        return -1;
    }
    rc = cloud_milli(cJSON_GetObjectItemCaseSensitive(data, "wind"),
                     "wind_speed", &milli, unit, sizeof unit);
    if (rc < 0 || (rc > 0 && take_wind_milli(out, OV_MEAS_WIND_SPEED, &out->wind_mms,
                                             rc, milli, unit) < 0)) {
        if (why)
            *why = "wind speed";
        return -1;
    }
    rc = cloud_milli(cJSON_GetObjectItemCaseSensitive(data, "wind"),
                     "wind_direction", &milli, unit, sizeof unit);
    if (rc < 0 || (rc > 0 && take_dir_milli(out, rc, milli) < 0)) {
        if (why)
            *why = "wind direction";
        return -1;
    }
    take_cloud_group(out, data);
    if (take_cloud_rain(out, data, "rain_rate", OV_MEAS_RAIN_RATE, &out->rain_rate_umh, 1) != 0) {
        if (why)
            *why = "rain rate";
        return -1;
    }
    (void)take_cloud_rain(out, data, "hourly", OV_MEAS_RAIN_HOUR, &out->rain_hour_um, 0);
    (void)take_cloud_rain(out, data, "daily", OV_MEAS_RAIN_DAY, &out->rain_day_um, 0);
    (void)take_cloud_rain(out, data, "weekly", OV_MEAS_RAIN_WEEK, &out->rain_week_um, 0);
    (void)take_cloud_rain(out, data, "monthly", OV_MEAS_RAIN_MONTH, &out->rain_month_um, 0);
    if (cloud_state_on(cJSON_GetObjectItemCaseSensitive(data, "rainfall_piezo")))
        out->raining = true;
    note_rain(out);
    if (!out->has) {
        if (why)
            *why = "no weather data";
        return -1;
    }
    out->ok = true;
    if (why)
        *why = NULL;
    return 0;
}

static int parse_local(cJSON *root, ov_sample_t *out, const char **why)
{
    const char *msg = NULL;
    int bad = 0;

    take_common(out, cJSON_GetObjectItemCaseSensitive(root, "common_list"), &msg, &bad);
    if (bad) {
        if (why)
            *why = msg ? msg : "malformed";
        return -1;
    }
    if (take_rain_id(out, root, "0x0E", OV_MEAS_RAIN_RATE, &out->rain_rate_umh, 1) != 0) {
        if (why)
            *why = "rain rate";
        return -1;
    }
    (void)take_rain_id(out, root, "0x10", OV_MEAS_RAIN_DAY, &out->rain_day_um, 0);
    (void)take_rain_id(out, root, "0x11", OV_MEAS_RAIN_WEEK, &out->rain_week_um, 0);
    (void)take_rain_id(out, root, "0x12", OV_MEAS_RAIN_MONTH, &out->rain_month_um, 0);
    take_wh25(out, root);
    take_local_extra(out, root);
    note_rain(out);
    if (!out->has) {
        if (why)
            *why = "no weather data";
        return -1;
    }
    out->ok = true;
    if (why)
        *why = NULL;
    return 0;
}

int wx_eco_parse(const char *json, ov_sample_t *out, const char **why)
{
    cJSON *root;
    cJSON *data;
    int rc;

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
    if (api_rejected(root)) {
        cJSON_Delete(root);
        if (why)
            *why = "ecowitt api";
        return -1;
    }
    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsObject(data) &&
        (cJSON_GetObjectItemCaseSensitive(data, "outdoor") ||
         cJSON_GetObjectItemCaseSensitive(data, "wind")))
        rc = parse_cloud(data, out, why);
    else
        rc = parse_local(root, out, why);
    cJSON_Delete(root);
    return rc;
}

#if !defined(OV_HOST)
static int eco_cloud_url(char *dst, size_t n, const wx_cfg_t *cfg)
{
    int k;
    if (!cfg || !cfg->app_key[0] || !cfg->api_key[0] || !cfg->station_id[0])
        return -1;
    k = snprintf(dst, n,
                 "https://api.ecowitt.net/api/v3/device/real_time"
                 "?application_key=%s&api_key=%s&mac=%s&call_back=all"
                 "&temp_unitid=1&pressure_unitid=3&wind_speed_unitid=6"
                 "&rainfall_unitid=12&solar_irradiance_unitid=16",
                 cfg->app_key, cfg->api_key, cfg->station_id);
    if (k < 0 || (size_t)k >= n)
        return -1;
    return 0;
}

esp_err_t wx_eco_fetch(const wx_cfg_t *cfg, int sub, ov_sample_t *out,
                       int timeout_ms, const char **why)
{
    char url[512];

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
        if (snprintf(url, sizeof url, "http://%s:%u/get_livedata_info",
                     cfg->host, (unsigned)cfg->port) >= (int)sizeof url) {
            if (why)
                *why = "station address";
            return ESP_ERR_INVALID_ARG;
        }
        return wx_fetch_url(url, NULL, NULL, timeout_ms, wx_eco_parse, out, why, "ecowitt");
    }
    if (cfg->url[0])
        return wx_fetch_url(cfg->url, NULL, NULL, timeout_ms, wx_eco_parse, out, why, "ecowitt");
    if (eco_cloud_url(url, sizeof url, cfg) != 0) {
        if (why)
            *why = "station address";
        return ESP_ERR_INVALID_ARG;
    }
    return wx_fetch_url(url, NULL, NULL, timeout_ms, wx_eco_parse, out, why, "ecowitt");
}
#endif
