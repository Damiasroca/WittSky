#pragma once

#include "overlay_wx.h"

#include <stddef.h>
#include <stdint.h>

/* milli is the value times 1000. unit may be empty. */
int ov_parse_milli(const char *s, int32_t *milli, char *unit, size_t unit_n);

/* Empty wind unit means m/s, the gateway protocol's base unit. */
int ov_wind_to_mms(int32_t milli, const char *unit, int32_t *mms);
/* Empty temperature unit means Celsius. */
int ov_temp_to_mc(int32_t milli, const char *unit, int32_t *mc);
/* Empty pressure unit means hPa. Empty rain unit means millimetres.
 * Empty humidity unit means percent. Distance: km, or miles.
 */
int ov_press_to_pa(int32_t milli, const char *unit, int32_t *pa);
int ov_rain_to_um(int32_t milli, const char *unit, int32_t *um);
int ov_hum_to_pm(int32_t milli, const char *unit, int32_t *pm);
int ov_dist_to_m(int32_t milli, const char *unit, int32_t *meters);

int32_t ov_wind_tenths(int32_t wind_mms, int unit);
int32_t ov_temp_tenths(int32_t temp_mc, int unit);

const char *ov_card16(int dir_deg);
int ov_fmt_wind(char *dst, size_t n, int32_t wind_mms, int dir_deg, int unit);
int ov_fmt_temp(char *dst, size_t n, int32_t temp_mc, int unit);

/* Writes "-" when the sample does not carry that measurement. */
int ov_fmt_measure(char *dst, size_t n, int meas, const ov_sample_t *wx,
                   int wind_unit, int temp_unit, int press_unit, int rain_unit);
