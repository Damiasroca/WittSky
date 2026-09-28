#pragma once

#include "overlay_wx.h"

/* Local GET /get_livedata_info, or the cloud
 * GET /api/v3/device/real_time document.
 *
 * Cloud fields: data.outdoor.temperature, data.wind.wind_speed,
 * data.wind.wind_direction. Raining when data.rainfall.rain_rate or
 * data.rainfall_piezo.rain_rate is a positive number, or
 * data.rainfall_piezo.state is 1. A missing rain object is not rain.
 * A present rate that is not a number fails the parse. code must be 0.
 *
 * Ecowitt local GET /get_livedata_info.
 * Outdoor temperature is common_list id 0x02. Wind speed is 0x0B
 * (instantaneous, not gust 0x0C). Direction is 0x0A. Feel-like id "3"
 * is ignored. A missing unit on temperature is Celsius, and a missing
 * unit on wind speed is m/s.
 *
 * Raining when rain id 0x0E or piezoRain id 0x0E is a positive rate, in
 * whatever unit the gateway appended (mm/h, in/h, ...). A positive rate
 * in either array counts. piezoRain id srain_piezo equal to 1 also counts.
 * If none of those fields are present, it is not raining. A rain-rate
 * field that is present but not a number fails the parse.
 * Temperature, wind speed, and direction are required.
 */
int ov_eco_parse(const char *json, ov_sample_t *out, const char **why);
