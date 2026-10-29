#pragma once

#include "esp_err.h"
#include "overlay_wx.h"

esp_err_t wx_fetch(int prov, int sub, const wx_cfg_t *cfg, ov_sample_t *out,
                   int timeout_ms, const char **why);
