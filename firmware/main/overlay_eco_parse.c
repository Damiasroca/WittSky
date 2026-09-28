#include "overlay_eco_parse.h"

#include "cJSON.h"
#include "overlay_units.h"

#include <stdio.h>
#include <string.h>

static void snprintf_num(char *dst, size_t n, double v)
{
    int ip;
    int frac;
    int neg = v < 0;
    if (neg)
        v = -v;
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

static int rate_positive(cJSON *root, const char *key, int *bad)
{
    cJSON *it = find_id(cJSON_GetObjectItemCaseSensitive(root, key), "0x0E");
    int32_t milli = 0;
    char unit[16];
    if (!it)
        return 0;
    if (item_measure(it, &milli, unit, sizeof unit) != 0) {
        *bad = 1;
        return 0;
    }
    return milli > 0;
}

static int piezo_flag(cJSON *root)
{
    cJSON *it = find_id(cJSON_GetObjectItemCaseSensitive(root, "piezoRain"),
                        "srain_piezo");
    cJSON *val;
    if (!it)
        return 0;
    val = cJSON_GetObjectItemCaseSensitive(it, "val");
    if (cJSON_IsString(val) && val->valuestring && val->valuestring[0] == '1' &&
        val->valuestring[1] == 0)
        return 1;
    if (cJSON_IsNumber(val) && val->valueint == 1)
        return 1;
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

static int cloud_positive(cJSON *parent, const char *key, int *bad)
{
    cJSON *it;
    int32_t milli = 0;
    char unit[16];

    if (!cJSON_IsObject(parent))
        return 0;
    it = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!it)
        return 0;
    if (cloud_point(it, &milli, unit, sizeof unit) != 0) {
        *bad = 1;
        return 0;
    }
    return milli > 0;
}

static int cloud_state_on(cJSON *piezo)
{
    cJSON *it;
    cJSON *val;

    if (!cJSON_IsObject(piezo))
        return 0;
    it = cJSON_GetObjectItemCaseSensitive(piezo, "state");
    if (!it)
        return 0;
    val = cJSON_GetObjectItemCaseSensitive(it, "value");
    if (cJSON_IsString(val) && val->valuestring && val->valuestring[0] == '1' &&
        val->valuestring[1] == 0)
        return 1;
    if (cJSON_IsNumber(val) && val->valueint == 1)
        return 1;
    return 0;
}

static int parse_v3(cJSON *data, ov_sample_t *out, const char **why)
{
    cJSON *outdoor = cJSON_GetObjectItemCaseSensitive(data, "outdoor");
    cJSON *wind = cJSON_GetObjectItemCaseSensitive(data, "wind");
    cJSON *it;
    int32_t milli = 0;
    int32_t mms = 0;
    int32_t mc = 0;
    char unit[16];
    int bad_rate = 0;

    it = cJSON_IsObject(outdoor) ? cJSON_GetObjectItemCaseSensitive(outdoor, "temperature") : NULL;
    if (!it || cloud_point(it, &milli, unit, sizeof unit) != 0 ||
        ov_temp_to_mc(milli, unit, &mc) != 0) {
        if (why)
            *why = "no outdoor temperature";
        return -1;
    }
    it = cJSON_IsObject(wind) ? cJSON_GetObjectItemCaseSensitive(wind, "wind_speed") : NULL;
    if (!it || cloud_point(it, &milli, unit, sizeof unit) != 0 ||
        ov_wind_to_mms(milli, unit, &mms) != 0) {
        if (why)
            *why = "no wind speed";
        return -1;
    }
    it = cJSON_GetObjectItemCaseSensitive(wind, "wind_direction");
    if (!it || cloud_point(it, &milli, unit, sizeof unit) != 0) {
        if (why)
            *why = "no wind direction";
        return -1;
    }
    {
        int deg = (int)((milli >= 0 ? milli + 500 : milli - 500) / 1000);
        if (deg < 0 || deg > 360) {
            if (why)
                *why = "wind direction";
            return -1;
        }
        out->dir_deg = deg % 360;
    }
    if (cloud_positive(cJSON_GetObjectItemCaseSensitive(data, "rainfall"), "rain_rate", &bad_rate) ||
        cloud_positive(cJSON_GetObjectItemCaseSensitive(data, "rainfall_piezo"), "rain_rate", &bad_rate) ||
        cloud_state_on(cJSON_GetObjectItemCaseSensitive(data, "rainfall_piezo")))
        out->raining = true;
    if (bad_rate) {
        if (why)
            *why = "rain rate";
        return -1;
    }
    out->wind_mms = mms;
    out->temp_mc = mc;
    out->ok = true;
    if (why)
        *why = NULL;
    return 0;
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

int ov_eco_parse(const char *json, ov_sample_t *out, const char **why)
{
    cJSON *root;
    cJSON *common;
    cJSON *data;
    cJSON *it;
    int32_t milli = 0;
    int32_t mms = 0;
    int32_t mc = 0;
    char unit[16];
    int bad_rate = 0;
    int raining;

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
         cJSON_GetObjectItemCaseSensitive(data, "wind"))) {
        int rc = parse_v3(data, out, why);
        cJSON_Delete(root);
        return rc;
    }
    common = cJSON_GetObjectItemCaseSensitive(root, "common_list");
    it = find_id(common, "0x02");
    if (!it || item_measure(it, &milli, unit, sizeof unit) != 0 ||
        ov_temp_to_mc(milli, unit, &mc) != 0) {
        cJSON_Delete(root);
        if (why)
            *why = "no outdoor temperature";
        return -1;
    }
    it = find_id(common, "0x0B");
    if (!it || item_measure(it, &milli, unit, sizeof unit) != 0 ||
        ov_wind_to_mms(milli, unit, &mms) != 0) {
        cJSON_Delete(root);
        if (why)
            *why = "no wind speed";
        return -1;
    }
    it = find_id(common, "0x0A");
    if (!it || item_measure(it, &milli, unit, sizeof unit) != 0) {
        cJSON_Delete(root);
        if (why)
            *why = "no wind direction";
        return -1;
    }
    {
        int deg = (int)((milli >= 0 ? milli + 500 : milli - 500) / 1000);
        if (deg < 0 || deg > 360) {
            cJSON_Delete(root);
            if (why)
                *why = "wind direction";
            return -1;
        }
        out->dir_deg = deg % 360;
    }
    raining = rate_positive(root, "rain", &bad_rate) ||
              rate_positive(root, "piezoRain", &bad_rate) ||
              piezo_flag(root);
    cJSON_Delete(root);
    if (bad_rate) {
        if (why)
            *why = "rain rate";
        return -1;
    }
    out->wind_mms = mms;
    out->temp_mc = mc;
    out->raining = raining;
    out->ok = true;
    if (why)
        *why = NULL;
    return 0;
}
