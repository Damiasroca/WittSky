#pragma once

#include "esp_err.h"
#include "overlay_wx.h"

esp_err_t wx_wl_fetch(const wx_cfg_t *cfg, int sub, ov_sample_t *out,
                      int timeout_ms, const char **why);
