#include <stddef.h>

#include "sky_metrics.h"

static int clamp_pct(int pct, int limit)
{
    if (pct < 0)
        return 0;
    if (pct > limit)
        return limit;
    return pct;
}

bool sky_measure(const uint8_t *rgb, int width, int height,
                 const sky_metric_in_t *in, uint8_t *gray,
                 sky_metric_out_t *out)
{
    if (!rgb || !in || !gray || !out || width < 1 || height < 1)
        return false;

    int x0 = clamp_pct(in->x * width / 100, width);
    int y0 = clamp_pct(in->y * height / 100, height);
    int x1 = clamp_pct((in->x + in->w) * width / 100, width);
    int y1 = clamp_pct((in->y + in->h) * height / 100, height);
    if (x1 < x0)
        x1 = x0;
    if (y1 < y0)
        y1 = y0;

    uint64_t sum_y = 0;
    uint32_t sat_n = 0, usable_n = 0, cloud_n = 0, rb_n = 0;
    double rb_sum = 0;
    int sat = in->sat_thr;
    int rb_thr = in->rb_x1000;

    for (int y = 0; y < height; y++) {
        bool in_row = y >= y0 && y < y1;
        for (int x = 0; x < width; x++) {
            const uint8_t *p = rgb + ((size_t)y * (size_t)width + (size_t)x) * 3;
            int r = p[0];
            int g = p[1];
            int b = p[2];
            int Y = (77 * r + 150 * g + 29 * b) >> 8;
            gray[(size_t)y * (size_t)width + (size_t)x] = (uint8_t)Y;
            sum_y += (uint64_t)Y;
            if (!in_row || x < x0 || x >= x1)
                continue;
            if (r >= sat || g >= sat || b >= sat) {
                sat_n++;
                continue;
            }
            if (b == 0) {
                /* No R/B. R > 0 counts as cloud; R and B both 0 is ignored. */
                if (r == 0)
                    continue;
                usable_n++;
                cloud_n++;
                continue;
            }
            usable_n++;
            rb_sum += (double)r / (double)b;
            rb_n++;
            if ((uint32_t)r * 1000u >= (uint32_t)rb_thr * (uint32_t)b)
                cloud_n++;
        }
    }

    uint32_t sky_px = (uint32_t)(x1 - x0) * (uint32_t)(y1 - y0);
    uint32_t npix = (uint32_t)width * (uint32_t)height;
    out->luma = (double)sum_y / (double)npix;
    out->sky_px = sky_px;
    out->sat_n = sat_n;
    out->usable_n = usable_n;
    out->cloud_n = cloud_n;
    out->rb_sum = rb_sum;
    out->rb_n = rb_n;
    out->cloud_ok = sky_px > 0 && (uint64_t)usable_n * 20u >= sky_px;

    out->sharp_ok = false;
    out->sharp = 0;
    if (width < 3 || height < 3)
        return true;

    int64_t sum = 0;
    uint64_t sumsq = 0;
    uint32_t n = 0;
    for (int y = 1; y < height - 1; y++) {
        for (int x = 1; x < width - 1; x++) {
            size_t i = (size_t)y * (size_t)width + (size_t)x;
            int lap = (int)gray[i - 1] + (int)gray[i + 1]
                    + (int)gray[i - (size_t)width] + (int)gray[i + (size_t)width]
                    - 4 * (int)gray[i];
            sum += lap;
            sumsq += (uint64_t)((int64_t)lap * lap);
            n++;
        }
    }
    if (n == 0)
        return true;
    double mean = (double)sum / (double)n;
    double var = (double)sumsq / (double)n - mean * mean;
    if (var < 0)
        var = 0;
    out->sharp = var;
    out->sharp_ok = true;
    return true;
}
