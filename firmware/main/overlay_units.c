#include "overlay_units.h"

#include <stdio.h>
#include <string.h>

static void lower_copy(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    if (!src)
        src = "";
    while (src[i] && i + 1 < n) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        dst[i++] = c;
    }
    dst[i] = 0;
}

int ov_parse_milli(const char *s, int32_t *milli, char *unit, size_t unit_n)
{
    int sign = 1;
    int digits = 0;
    int32_t ip = 0;
    int32_t frac = 0;
    int div = 1;

    if (!s || !milli || !unit || unit_n == 0)
        return -1;
    unit[0] = 0;
    while (*s == ' ')
        s++;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    if (*s < '0' || *s > '9')
        return -1;
    while (*s >= '0' && *s <= '9') {
        if (digits >= 6)
            return -1;
        ip = ip * 10 + (*s - '0');
        s++;
        digits++;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            if (div < 1000) {
                frac = frac * 10 + (*s - '0');
                div *= 10;
            }
            s++;
        }
    }
    *milli = sign * (ip * 1000 + (div > 1 ? frac * 1000 / div : 0));
    while (*s == ' ')
        s++;
    size_t i = 0;
    while (*s && *s != ',' && *s != '"' && i + 1 < unit_n) {
        unit[i++] = *s++;
    }
    unit[i] = 0;
    return 0;
}

static int is_ms(const char *u)
{
    return strcmp(u, "m/s") == 0 || strcmp(u, "mps") == 0;
}

static int is_kmh(const char *u)
{
    return strcmp(u, "km/h") == 0 || strcmp(u, "km/hr") == 0 ||
           strcmp(u, "kmh") == 0 || strcmp(u, "kph") == 0;
}

static int is_mph(const char *u)
{
    return strcmp(u, "mph") == 0;
}

static int is_kn(const char *u)
{
    return strcmp(u, "kn") == 0 || strcmp(u, "kt") == 0 ||
           strcmp(u, "kts") == 0 || strcmp(u, "knots") == 0;
}

int ov_wind_to_mms(int32_t milli, const char *unit, int32_t *mms)
{
    char u[16];
    if (!mms || milli < 0)
        return -1;
    lower_copy(u, sizeof u, unit);
    if (!u[0] || is_ms(u)) {
        *mms = milli;
        return 0;
    }
    if (is_kmh(u)) {
        *mms = (int32_t)((int64_t)milli * 5 / 18);
        return 0;
    }
    if (is_mph(u)) {
        *mms = (int32_t)((int64_t)milli * 44704 / 100000);
        return 0;
    }
    if (is_kn(u)) {
        *mms = (int32_t)((int64_t)milli * 514444 / 1000000);
        return 0;
    }
    return -1;
}

static int is_c(const char *u)
{
    return strcmp(u, "c") == 0 || strcmp(u, "\xc2\xb0" "c") == 0 ||
           strcmp(u, "\xc2\xba" "c") == 0 || strcmp(u, "\xe2\x84\x83") == 0;
}

static int is_f(const char *u)
{
    return strcmp(u, "f") == 0 || strcmp(u, "\xc2\xb0" "f") == 0 ||
           strcmp(u, "\xc2\xba" "f") == 0 || strcmp(u, "\xe2\x84\x89") == 0;
}

int ov_temp_to_mc(int32_t milli, const char *unit, int32_t *mc)
{
    char u[16];
    if (!mc)
        return -1;
    lower_copy(u, sizeof u, unit);
    if (!u[0] || is_c(u)) {
        *mc = milli;
        return 0;
    }
    if (is_f(u)) {
        *mc = (int32_t)(((int64_t)milli - 32000) * 5 / 9);
        return 0;
    }
    return -1;
}

int32_t ov_wind_tenths(int32_t wind_mms, int unit)
{
    if (wind_mms < 0)
        wind_mms = 0;
    switch (unit) {
    case 1:
        return (wind_mms + 50) / 100;
    case 2:
        return (int32_t)(((int64_t)wind_mms * 22369 + 500000) / 1000000);
    case 3:
        return (int32_t)(((int64_t)wind_mms * 19438 + 500000) / 1000000);
    default:
        return (int32_t)(((int64_t)wind_mms * 36 + 500) / 1000);
    }
}

static int32_t round_tenths_from_milli(int64_t milli)
{
    if (milli >= 0)
        return (int32_t)((milli + 50) / 100);
    return (int32_t)(-(((-milli) + 50) / 100));
}

int32_t ov_temp_tenths(int32_t temp_mc, int unit)
{
    if (unit == 1) {
        int64_t f_milli = (int64_t)temp_mc * 9 / 5 + 32000;
        return round_tenths_from_milli(f_milli);
    }
    return round_tenths_from_milli(temp_mc);
}

const char *ov_card16(int dir_deg)
{
    static const char *name[] = {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
        "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW",
    };
    int d = dir_deg % 360;
    if (d < 0)
        d += 360;
    int i = (d * 10 + 112) / 225;
    if (i >= 16)
        i = 0;
    return name[i];
}

static const char *wind_unit_name(int unit)
{
    switch (unit) {
    case 1: return "m/s";
    case 2: return "mph";
    case 3: return "kn";
    default: return "km/h";
    }
}

static void fmt_tenths(char *dst, size_t n, int32_t tenths)
{
    int neg = tenths < 0;
    int32_t v = neg ? -tenths : tenths;
    snprintf(dst, n, "%s%d.%d", neg ? "-" : "", (int)(v / 10), (int)(v % 10));
}

int ov_fmt_wind(char *dst, size_t n, int32_t wind_mms, int dir_deg, int unit)
{
    char num[16];
    if (!dst || n == 0)
        return -1;
    fmt_tenths(num, sizeof num, ov_wind_tenths(wind_mms, unit));
    return snprintf(dst, n, "%s %s %s", num, wind_unit_name(unit), ov_card16(dir_deg));
}

int ov_fmt_temp(char *dst, size_t n, int32_t temp_mc, int unit)
{
    char num[16];
    if (!dst || n == 0)
        return -1;
    fmt_tenths(num, sizeof num, ov_temp_tenths(temp_mc, unit));
    return snprintf(dst, n, "%s\xC2\xBA%s", num, unit == 1 ? "F" : "C");
}

static int32_t div_round(int64_t n, int64_t d)
{
    if (d < 0) {
        n = -n;
        d = -d;
    }
    if (n >= 0)
        return (int32_t)((n + d / 2) / d);
    return (int32_t)-(((-n) + d / 2) / d);
}

static void canon_unit(char *dst, size_t n, const char *src, int cut_slash)
{
    size_t i = 0;
    char *slash;
    if (!src)
        src = "";
    while (*src && i + 1 < n) {
        char c = *src++;
        if (c == ' ' || c == '_')
            continue;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        dst[i++] = c;
    }
    dst[i] = 0;
    if (!cut_slash)
        return;
    slash = strchr(dst, '/');
    if (slash)
        *slash = 0;
}

static int is_hpa(const char *u)
{
    return !u[0] || strcmp(u, "hpa") == 0 || strcmp(u, "mb") == 0 ||
           strcmp(u, "mbar") == 0 || strcmp(u, "millibar") == 0;
}

static int is_inhg(const char *u)
{
    return strcmp(u, "inhg") == 0 || strcmp(u, "in") == 0 ||
           strcmp(u, "inhg.") == 0;
}

static int is_mmhg(const char *u)
{
    return strcmp(u, "mmhg") == 0;
}

int ov_press_to_pa(int32_t milli, const char *unit, int32_t *pa)
{
    char u[16];
    if (!pa || milli < 0)
        return -1;
    canon_unit(u, sizeof u, unit, 0);
    if (is_hpa(u)) {
        *pa = div_round(milli, 10);
        return 0;
    }
    if (strcmp(u, "pa") == 0) {
        *pa = div_round(milli, 1000);
        return 0;
    }
    if (is_inhg(u)) {
        *pa = div_round((int64_t)milli * 3386389, 1000000);
        return 0;
    }
    if (is_mmhg(u)) {
        *pa = div_round((int64_t)milli * 101325, 760000);
        return 0;
    }
    return -1;
}

static int is_mm(const char *u)
{
    return !u[0] || strcmp(u, "mm") == 0 || strcmp(u, "millimeter") == 0 ||
           strcmp(u, "millimeters") == 0;
}

static int is_inch(const char *u)
{
    return strcmp(u, "in") == 0 || strcmp(u, "inch") == 0 ||
           strcmp(u, "inches") == 0;
}

int ov_rain_to_um(int32_t milli, const char *unit, int32_t *um)
{
    char u[16];
    if (!um || milli < 0)
        return -1;
    canon_unit(u, sizeof u, unit, 1);
    if (is_mm(u)) {
        *um = milli;
        return 0;
    }
    if (is_inch(u)) {
        *um = div_round((int64_t)milli * 25400, 1000);
        return 0;
    }
    return -1;
}

int ov_hum_to_pm(int32_t milli, const char *unit, int32_t *pm)
{
    char u[16];
    if (!pm || milli < 0)
        return -1;
    canon_unit(u, sizeof u, unit, 0);
    if (!u[0] || strcmp(u, "%") == 0 || strcmp(u, "rh") == 0 ||
        strcmp(u, "pct") == 0) {
        *pm = div_round(milli, 100);
        return 0;
    }
    return -1;
}

int ov_dist_to_m(int32_t milli, const char *unit, int32_t *meters)
{
    char u[16];
    if (!meters || milli < 0)
        return -1;
    canon_unit(u, sizeof u, unit, 0);
    if (!u[0] || strcmp(u, "km") == 0 || strcmp(u, "kilometer") == 0 ||
        strcmp(u, "kilometers") == 0) {
        *meters = milli;
        return 0;
    }
    if (strcmp(u, "mi") == 0 || strcmp(u, "mile") == 0 ||
        strcmp(u, "miles") == 0) {
        *meters = div_round((int64_t)milli * 1609344, 1000000);
        return 0;
    }
    if (strcmp(u, "m") == 0 || strcmp(u, "meter") == 0 ||
        strcmp(u, "meters") == 0) {
        *meters = div_round(milli, 1000);
        return 0;
    }
    return -1;
}

static void fmt_frac(char *dst, size_t n, int32_t scaled, int digits)
{
    int neg = scaled < 0;
    int32_t v = neg ? -scaled : scaled;
    int div = 1;
    int i;
    for (i = 0; i < digits; i++)
        div *= 10;
    if (div < 1)
        div = 1;
    snprintf(dst, n, "%s%d.%0*d", neg ? "-" : "", (int)(v / div), digits, (int)(v % div));
}

static int dash(char *dst, size_t n)
{
    return snprintf(dst, n, "-");
}

static int fmt_temp_unit(char *dst, size_t n, int32_t mc, int unit)
{
    char num[16];
    fmt_tenths(num, sizeof num, ov_temp_tenths(mc, unit));
    return snprintf(dst, n, "%s\xC2\xBA%s", num, unit == 1 ? "F" : "C");
}

static int fmt_wind_unit(char *dst, size_t n, int32_t mms, int unit)
{
    char num[16];
    fmt_tenths(num, sizeof num, ov_wind_tenths(mms, unit));
    return snprintf(dst, n, "%s %s", num, wind_unit_name(unit));
}

static int fmt_hum(char *dst, size_t n, int32_t pm)
{
    char num[16];
    fmt_frac(num, sizeof num, pm, 1);
    return snprintf(dst, n, "%s%%", num);
}

static int fmt_rain(char *dst, size_t n, int32_t um, int unit, int rate)
{
    char num[16];
    if (unit == 1) {
        fmt_frac(num, sizeof num, div_round(um, 254), 2);
        return snprintf(dst, n, "%s in%s", num, rate ? "/h" : "");
    }
    fmt_frac(num, sizeof num, div_round(um, 100), 1);
    return snprintf(dst, n, "%s mm%s", num, rate ? "/h" : "");
}

static int fmt_press(char *dst, size_t n, int32_t pa, int unit)
{
    char num[16];
    if (unit == 1) {
        fmt_frac(num, sizeof num, div_round((int64_t)pa * 100000, 3386389), 2);
        return snprintf(dst, n, "%s inHg", num);
    }
    if (unit == 2) {
        fmt_frac(num, sizeof num, div_round((int64_t)pa * 7600, 101325), 1);
        return snprintf(dst, n, "%s mmHg", num);
    }
    fmt_frac(num, sizeof num, div_round(pa, 10), 1);
    return snprintf(dst, n, "%s hPa", num);
}

int ov_fmt_measure(char *dst, size_t n, int meas, const ov_sample_t *wx,
                   int wind_unit, int temp_unit, int press_unit, int rain_unit)
{
    char num[16];
    if (!dst || n == 0)
        return -1;
    if (!wx || !(wx->has & OV_HAS(meas)))
        return dash(dst, n);
    switch (meas) {
    case OV_MEAS_WIND_SPEED:
        return fmt_wind_unit(dst, n, wx->wind_mms, wind_unit);
    case OV_MEAS_WIND_GUST:
        return fmt_wind_unit(dst, n, wx->gust_mms, wind_unit);
    case OV_MEAS_WIND_DIR:
        return snprintf(dst, n, "%d\xC2\xBA %s", wx->dir_deg, ov_card16(wx->dir_deg));
    case OV_MEAS_TEMP_OUT:
        return fmt_temp_unit(dst, n, wx->temp_out_mc, temp_unit);
    case OV_MEAS_TEMP_IN:
        return fmt_temp_unit(dst, n, wx->temp_in_mc, temp_unit);
    case OV_MEAS_HUM_OUT:
        return fmt_hum(dst, n, wx->hum_out_pm);
    case OV_MEAS_HUM_IN:
        return fmt_hum(dst, n, wx->hum_in_pm);
    case OV_MEAS_FEELS:
        return fmt_temp_unit(dst, n, wx->feels_mc, temp_unit);
    case OV_MEAS_DEW:
        return fmt_temp_unit(dst, n, wx->dew_mc, temp_unit);
    case OV_MEAS_PRESS_REL:
        return fmt_press(dst, n, wx->press_rel_pa, press_unit);
    case OV_MEAS_PRESS_ABS:
        return fmt_press(dst, n, wx->press_abs_pa, press_unit);
    case OV_MEAS_RAIN_RATE:
        return fmt_rain(dst, n, wx->rain_rate_umh, rain_unit, 1);
    case OV_MEAS_RAIN_HOUR:
        return fmt_rain(dst, n, wx->rain_hour_um, rain_unit, 0);
    case OV_MEAS_RAIN_DAY:
        return fmt_rain(dst, n, wx->rain_day_um, rain_unit, 0);
    case OV_MEAS_RAIN_WEEK:
        return fmt_rain(dst, n, wx->rain_week_um, rain_unit, 0);
    case OV_MEAS_RAIN_MONTH:
        return fmt_rain(dst, n, wx->rain_month_um, rain_unit, 0);
    case OV_MEAS_UV:
        fmt_frac(num, sizeof num, wx->uv_x10, 1);
        return snprintf(dst, n, "%s", num);
    case OV_MEAS_SOLAR:
        return snprintf(dst, n, "%d W/m2", (int)wx->solar_wm2);
    case OV_MEAS_LTNG_DIST:
        if (wind_unit == 2) {
            fmt_frac(num, sizeof num, div_round((int64_t)wx->ltng_dist_m * 10000, 1609344), 1);
            return snprintf(dst, n, "%s mi", num);
        }
        fmt_frac(num, sizeof num, div_round(wx->ltng_dist_m, 100), 1);
        return snprintf(dst, n, "%s km", num);
    case OV_MEAS_LTNG_DAY:
        return snprintf(dst, n, "%d", (int)wx->ltng_day);
    case OV_MEAS_PM25:
        fmt_frac(num, sizeof num, wx->pm25_x10, 1);
        return snprintf(dst, n, "%s ug/m3", num);
    case OV_MEAS_PM10:
        fmt_frac(num, sizeof num, wx->pm10_x10, 1);
        return snprintf(dst, n, "%s ug/m3", num);
    case OV_MEAS_CO2:
        return snprintf(dst, n, "%d ppm", (int)wx->co2_ppm);
    case OV_MEAS_AQI:
        return snprintf(dst, n, "%d", (int)wx->aqi);
    case OV_MEAS_SOIL_M:
        if (wx->soil_cb) {
            fmt_frac(num, sizeof num, div_round(wx->soil_m, 100), 1);
            return snprintf(dst, n, "%s cb", num);
        }
        return fmt_hum(dst, n, wx->soil_m);
    case OV_MEAS_SOIL_T:
        return fmt_temp_unit(dst, n, wx->soil_t_mc, temp_unit);
    case OV_MEAS_LEAF:
        if (wx->leaf_raw)
            return snprintf(dst, n, "%d", (int)wx->leaf);
        return fmt_hum(dst, n, wx->leaf);
    default:
        return dash(dst, n);
    }
}
