#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HP10_SKY_JSON_MAX 512

esp_err_t hp10_sky_compute(const uint8_t *jpeg, size_t jpeg_len,
                           uint16_t frame_w, uint16_t frame_h,
                           char *json_out, size_t json_out_n);
bool hp10_sky_latest(char *dst, size_t n);
