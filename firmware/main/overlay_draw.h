#pragma once

#include "overlay_font.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *rgb;
    int frame_w;
    int y0;
    int rows;
} ov_band_t;

void ov_fill_rect(ov_band_t *b, int x, int y, int w, int h,
                  int r, int g, int bl, int a);
void ov_draw_text(ov_band_t *b, const ov_font_t *font, int x, int y,
                  const char *utf8);
int ov_text_width(const ov_font_t *font, const char *utf8);
const ov_font_t *ov_font_pick(int frame_h);

/* 8-bit alpha, gray ink. src_w/src_h is the alpha plane. */
void ov_blit_alpha(ov_band_t *b, const uint8_t *alpha, int src_w, int src_h,
                   int dx, int dy, int dw, int dh,
                   int r, int g, int bl);

/* Rain icon: RGB565 pixels and 4-bit alpha, two pixels per byte. */
void ov_blit_rain(ov_band_t *b, int dx, int dy, int dw, int dh);

/* dir_deg is clockwise from north. radius is the degree-ring radius;
 * the arrow tip sits on that ring and points outward. wind_mms colours
 * the arrow from blue at 0 to red at 70 km/h. */
void ov_draw_needle(ov_band_t *b, int cx, int cy, int dir_deg, int radius,
                    int32_t wind_mms);

int ov_rose_inflate(uint8_t *dst, size_t dst_n);
