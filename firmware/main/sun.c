#include <math.h>

#include "sun.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double d2r(double d)
{
    return d * M_PI / 180.0;
}

static double r2d(double r)
{
    return r * 180.0 / M_PI;
}

static double wrap360(double x)
{
    x = fmod(x, 360.0);
    if (x < 0)
        x += 360.0;
    return x;
}

static double wrap24(double h)
{
    h = fmod(h, 24.0);
    if (h < 0)
        h += 24.0;
    return h;
}

/* Ed Williams / USNO: zenith 90.833°. hours is 6=rise, 18=set. */
static bool sun_one(double lat, double lon, int y, int mo, int d,
                    int32_t utc_off, int hours, int *out_min)
{
    static const int mdays[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int doy = mdays[mo - 1] + d;
    if (mo > 2 && ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)))
        doy++;

    double lonh = lon / 15.0;
    double t = doy + ((double)hours - lonh) / 24.0;
    double M = 0.9856 * t - 3.289;
    double L = wrap360(M + 1.916 * sin(d2r(M)) + 0.020 * sin(d2r(2 * M)) + 282.634);
    double RA = r2d(atan(0.91764 * tan(d2r(L))));
    RA = wrap360(RA);
    RA += floor(L / 90.0) * 90.0 - floor(RA / 90.0) * 90.0;
    RA /= 15.0;
    double sinDec = 0.39782 * sin(d2r(L));
    double cosDec = cos(asin(sinDec));
    double cosH = (cos(d2r(90.833)) - sinDec * sin(d2r(lat))) /
                  (cosDec * cos(d2r(lat)));
    if (cosH > 1.0 || cosH < -1.0)
        return false;
    double H = (hours == 6) ? (360.0 - r2d(acos(cosH))) : r2d(acos(cosH));
    H /= 15.0;
    double UT = wrap24(H + RA - 0.06571 * t - 6.622 - lonh);
    double local = wrap24(UT + utc_off / 3600.0);
    int mins = (int)(local * 60.0 + 0.5);
    if (mins >= 24 * 60)
        mins = 0;
    *out_min = mins;
    return true;
}

bool sun_rise_set(double lat, double lon, int y, int mo, int d,
                  int32_t utc_off, int *rise_min, int *set_min)
{
    int rise = 0, set = 0;
    if (!sun_one(lat, lon, y, mo, d, utc_off, 6, &rise) ||
        !sun_one(lat, lon, y, mo, d, utc_off, 18, &set))
        return false;
    if (rise_min)
        *rise_min = rise;
    if (set_min)
        *set_min = set;
    return true;
}
