#include "overlay_meas.h"

static const char *const s_name[OV_MEAS_N] = {
    "Wind",
    "Gust",
    "Wind direction",
    "Temperature",
    "Indoor temperature",
    "Humidity",
    "Indoor humidity",
    "Feels like",
    "Dew point",
    "Pressure",
    "Absolute pressure",
    "Rain rate",
    "Hourly rain",
    "Daily rain",
    "Weekly rain",
    "Monthly rain",
    "UV index",
    "Solar",
    "Lightning distance",
    "Lightning strikes",
    "PM2.5",
    "PM10",
    "CO2",
    "AQI",
    "Soil moisture",
    "Soil temperature",
    "Leaf wetness",
};

const char *ov_meas_name(int id)
{
    if (id < 0 || id >= OV_MEAS_N)
        return "?";
    return s_name[id];
}

static uint32_t all_but(int drop)
{
    uint32_t m = (1u << OV_MEAS_N) - 1;
    if (drop >= 0 && drop < OV_MEAS_N)
        m &= ~OV_HAS(drop);
    return m;
}

static uint32_t listed(const int *id, int n)
{
    uint32_t m = 0;
    for (int i = 0; i < n; i++) {
        if (id[i] >= 0 && id[i] < OV_MEAS_N)
            m |= OV_HAS(id[i]);
    }
    return m;
}

uint32_t ov_meas_mask(int prov, int sub)
{
    /* WeatherLink: no weekly rain, no lightning. Cloud adds AirLink PM. */
    static const int wl_local[] = {
        OV_MEAS_WIND_SPEED, OV_MEAS_WIND_GUST, OV_MEAS_WIND_DIR,
        OV_MEAS_TEMP_OUT, OV_MEAS_TEMP_IN, OV_MEAS_HUM_OUT, OV_MEAS_HUM_IN,
        OV_MEAS_FEELS, OV_MEAS_DEW, OV_MEAS_PRESS_REL, OV_MEAS_PRESS_ABS,
        OV_MEAS_RAIN_RATE, OV_MEAS_RAIN_HOUR, OV_MEAS_RAIN_DAY, OV_MEAS_RAIN_MONTH,
        OV_MEAS_UV, OV_MEAS_SOLAR, OV_MEAS_SOIL_M, OV_MEAS_SOIL_T, OV_MEAS_LEAF,
    };
    static const int tempest[] = {
        OV_MEAS_WIND_SPEED, OV_MEAS_WIND_GUST, OV_MEAS_WIND_DIR,
        OV_MEAS_TEMP_OUT, OV_MEAS_HUM_OUT, OV_MEAS_FEELS, OV_MEAS_PRESS_ABS,
        OV_MEAS_RAIN_RATE, OV_MEAS_RAIN_HOUR, OV_MEAS_RAIN_DAY,
        OV_MEAS_UV, OV_MEAS_SOLAR, OV_MEAS_LTNG_DIST,
    };
    static const int wu[] = {
        OV_MEAS_WIND_SPEED, OV_MEAS_WIND_GUST, OV_MEAS_WIND_DIR,
        OV_MEAS_TEMP_OUT, OV_MEAS_HUM_OUT, OV_MEAS_FEELS, OV_MEAS_DEW,
        OV_MEAS_PRESS_REL, OV_MEAS_RAIN_RATE, OV_MEAS_RAIN_DAY,
        OV_MEAS_UV, OV_MEAS_SOLAR,
    };
    uint32_t wl;

    switch (prov) {
    case OV_PROV_ECO:
        /* Local live data has no hourly total. Cloud real_time does. */
        if (sub == OV_SUB_LOCAL)
            return all_but(OV_MEAS_RAIN_HOUR);
        return all_but(-1);
    case OV_PROV_WL:
        wl = listed(wl_local, (int)(sizeof wl_local / sizeof wl_local[0]));
        if (sub == OV_SUB_CLOUD)
            wl |= OV_HAS(OV_MEAS_PM25) | OV_HAS(OV_MEAS_PM10);
        return wl;
    case OV_PROV_AWN:
        return all_but(OV_MEAS_RAIN_HOUR);
    case OV_PROV_TEMPEST:
        return listed(tempest, (int)(sizeof tempest / sizeof tempest[0]));
    case OV_PROV_WU:
        return listed(wu, (int)(sizeof wu / sizeof wu[0]));
    default:
        return 0;
    }
}
