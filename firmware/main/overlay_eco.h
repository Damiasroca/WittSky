#pragma once

#include "esp_err.h"
#include "overlay_wx.h"

esp_err_t ov_eco_fetch(ov_sample_t *out, const char *host, int port,
                       int timeout_ms, const char **why);
esp_err_t ov_eco_fetch_url(ov_sample_t *out, const char *url,
                           int timeout_ms, const char **why);
