#pragma once

#include "sky_metrics.h"
#include "sky_sensor.h"

#include <stdint.h>

char *sky_format_json(uint16_t frame_w, uint16_t frame_h,
                      uint16_t dec_w, uint16_t dec_h,
                      const sky_metric_in_t *in, const sky_metric_out_t *m,
                      const sky_sensor_t *sensor, int day, int ms);
