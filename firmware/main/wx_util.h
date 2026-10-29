#pragma once

#include "cJSON.h"
#include "overlay_wx.h"

/* Read a numeric or stringified-with-unit value as a signed milli-unit
 * integer. Numbers scale by 1000; strings go through ov_parse_milli so a
 * trailing unit like "6.3 m/s" is tolerated. Returns 0 on success.
 */
int wx_num(cJSON *obj, const char *key, int32_t *out);

/* Scale a double to milli-units for direct callers that bypass wx_num. */
int32_t wx_milli(double v);

int wx_conv_temp_f(int32_t m, int32_t *out);
int wx_conv_temp_c(int32_t m, int32_t *out);
int wx_conv_wind_mph(int32_t m, int32_t *out);
int wx_conv_wind_ms(int32_t m, int32_t *out);
int wx_conv_wind_kmh(int32_t m, int32_t *out);
int wx_conv_press_inhg(int32_t m, int32_t *out);
int wx_conv_press_hpa(int32_t m, int32_t *out);
int wx_conv_rain_in(int32_t m, int32_t *out);
int wx_conv_rain_mm(int32_t m, int32_t *out);
int wx_conv_hum(int32_t m, int32_t *out);
int wx_conv_x10(int32_t m, int32_t *out);
int wx_conv_wm2(int32_t m, int32_t *out);
int wx_conv_count(int32_t m, int32_t *out);
int wx_conv_km(int32_t m, int32_t *out);
int wx_conv_mi(int32_t m, int32_t *out);
int wx_conv_cb(int32_t m, int32_t *out);
int wx_conv_dir(int32_t m, int32_t *out);

void wx_take(ov_sample_t *o, cJSON *obj, const char *key, int meas,
             int32_t *slot, int (*conv)(int32_t m, int32_t *out));
void wx_take_dir(ov_sample_t *o, cJSON *obj, const char *key);

void wx_finish(ov_sample_t *o, const char **why);
