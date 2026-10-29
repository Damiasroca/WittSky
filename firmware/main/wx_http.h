#pragma once

#include "esp_err.h"
#include "overlay_wx.h"

#define WX_BODY_CAP 32768

/* GET url into a NUL-terminated body. hdr may be NULL.
 * parse returns 0 and fills *out, or -1 and sets *why.
 */
esp_err_t wx_fetch_url(const char *url, const char *hdr, const char *hdr_val,
                       int timeout_ms,
                       int (*parse)(const char *json, ov_sample_t *out, const char **why),
                       ov_sample_t *out, const char **why, const char *who);
