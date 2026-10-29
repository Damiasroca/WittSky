#pragma once

#include "esp_err.h"
#include "overlay_wx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct cJSON;

enum {
    OV_DEST_CUSTOM = 0,
    OV_DEST_ECO = 1,
    OV_DEST_STILL = 2
};

/* False when the master switch is off, and for an Ecowitt upload when
 * the clean-image option is on.
 */
bool hp10_overlay_active(int dest);

/* Call before taking the camera mutex. On success *run is true.
 * wx->ok is false when no station is selected; clock and rose still draw.
 * A failed fetch leaves *run false so the caller sends the original JPEG.
 */
void hp10_overlay_prepare(int dest, ov_sample_t *wx, bool *run);

/* wx may be NULL when there is no weather sample. The source JPEG is not
 * modified. On ESP_OK, *out is a new buffer the caller frees with free().
 * Any failure leaves *out NULL and the caller keeps the original JPEG.
 */
esp_err_t hp10_overlay_render(const ov_sample_t *wx,
                              const uint8_t *jpeg, size_t jpeg_len,
                              uint8_t **out, size_t *out_len);

/* Test hook. 0 off, 1 fail the weather fetch, 2 fail the output allocation. */
void hp10_overlay_debug_fail(int which);

/* Drop a cached station sample after the provider or credentials change. */
void hp10_overlay_cache_clear(void);

/* Last station fetch for /get_health. Probe hits the API and skips the cache. */
void hp10_overlay_wx_add_json(struct cJSON *o);
esp_err_t hp10_overlay_wx_probe(const char **why);
