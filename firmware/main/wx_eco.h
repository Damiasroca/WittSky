#pragma once

#include "overlay_wx.h"

/* Local GET /get_livedata_info, or cloud GET /api/v3/device/real_time.
 * Temperature, wind, and direction are used when present. A present value
 * that is not a number fails the parse for those three and for rain rate.
 * Other catalog fields are filled when the document carries them.
 * code, when present, must be 0.
 */
int wx_eco_parse(const char *json, ov_sample_t *out, const char **why);

#if !defined(OV_HOST)
#include "esp_err.h"

esp_err_t wx_eco_fetch(const wx_cfg_t *cfg, int sub, ov_sample_t *out,
                       int timeout_ms, const char **why);
#endif
