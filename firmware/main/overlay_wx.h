#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Internal units: wind in mm/s, temperature in milli-degrees Celsius,
 * direction in degrees clockwise from north. Display units are applied
 * only when the overlay text is built.
 */
typedef struct {
    int32_t wind_mms;
    int32_t temp_mc;
    int dir_deg;
    bool raining;
    bool ok;
    int fetch_ms;
} ov_sample_t;
