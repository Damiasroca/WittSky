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
