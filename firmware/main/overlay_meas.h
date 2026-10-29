#pragma once

#include <stdint.h>

/* Catalog order is the bit position in ov_sample_t.has and in the
 * provider masks. Keep the web page lists in overlay.html aligned.
 */
enum {
    OV_MEAS_WIND_SPEED = 0,
    OV_MEAS_WIND_GUST,
    OV_MEAS_WIND_DIR,
    OV_MEAS_TEMP_OUT,
    OV_MEAS_TEMP_IN,
    OV_MEAS_HUM_OUT,
    OV_MEAS_HUM_IN,
    OV_MEAS_FEELS,
    OV_MEAS_DEW,
    OV_MEAS_PRESS_REL,
    OV_MEAS_PRESS_ABS,
    OV_MEAS_RAIN_RATE,
    OV_MEAS_RAIN_HOUR,
    OV_MEAS_RAIN_DAY,
    OV_MEAS_RAIN_WEEK,
    OV_MEAS_RAIN_MONTH,
    OV_MEAS_UV,
    OV_MEAS_SOLAR,
    OV_MEAS_LTNG_DIST,
    OV_MEAS_LTNG_DAY,
    OV_MEAS_PM25,
    OV_MEAS_PM10,
    OV_MEAS_CO2,
    OV_MEAS_AQI,
    OV_MEAS_SOIL_M,
    OV_MEAS_SOIL_T,
    OV_MEAS_LEAF,
    OV_MEAS_N
};

#define OV_HAS(m) (1u << (m))

enum {
    OV_PROV_NONE = 0,
    OV_PROV_ECO = 1,
    OV_PROV_WL = 2,
    OV_PROV_AWN = 3,
    OV_PROV_TEMPEST = 4,
    OV_PROV_WU = 5,
    OV_PROV_N
};

enum {
    OV_SUB_LOCAL = 0,
    OV_SUB_CLOUD = 1
};

#define OV_SLOT_N 5

const char *ov_meas_name(int id);
uint32_t ov_meas_mask(int prov, int sub);
