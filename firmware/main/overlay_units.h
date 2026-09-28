#pragma once

#include <stddef.h>
#include <stdint.h>

/* milli is the value times 1000. unit may be empty. */
int ov_parse_milli(const char *s, int32_t *milli, char *unit, size_t unit_n);

/* Empty wind unit means m/s, the gateway protocol's base unit. */
int ov_wind_to_mms(int32_t milli, const char *unit, int32_t *mms);
/* Empty temperature unit means Celsius. */
int ov_temp_to_mc(int32_t milli, const char *unit, int32_t *mc);

int32_t ov_wind_tenths(int32_t wind_mms, int unit);
int32_t ov_temp_tenths(int32_t temp_mc, int unit);

const char *ov_card16(int dir_deg);
int ov_fmt_wind(char *dst, size_t n, int32_t wind_mms, int dir_deg, int unit);
int ov_fmt_temp(char *dst, size_t n, int32_t temp_mc, int unit);
