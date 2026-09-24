#pragma once

#include <stdbool.h>

typedef struct {
    bool read_ok;
    int aec;
    int gain_reg;
    double gain_x;
    bool light_ok;
    double light_idx;
    int awb_mode;
    bool gains_ok;
    int awb_r, awb_g, awb_b;
} sky_sensor_t;

void sky_sensor_read(double luma, sky_sensor_t *out);
