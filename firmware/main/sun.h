#pragma once

#include <stdbool.h>
#include <stdint.h>

bool sun_rise_set(double lat, double lon, int y, int mo, int d,
                  int32_t utc_off, int *rise_min, int *set_min);
