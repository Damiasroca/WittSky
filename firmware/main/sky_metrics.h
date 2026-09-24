#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int x, y, w, h;
    int sat_thr;
    int rb_x1000;
} sky_metric_in_t;

typedef struct {
    double luma;
    double sharp;
    bool sharp_ok;
    uint32_t sky_px;
    uint32_t sat_n;
    uint32_t usable_n;
    uint32_t cloud_n;
    double rb_sum;
    uint32_t rb_n;
    bool cloud_ok;
} sky_metric_out_t;

bool sky_measure(const uint8_t *rgb, int width, int height,
                 const sky_metric_in_t *in, uint8_t *gray,
                 sky_metric_out_t *out);
