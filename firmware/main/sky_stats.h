#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HP10_SKY_JSON_MAX 640

/* Mean R, G, B of the sky mask on a 1/8 JPEG decode. False when the mask is empty. */
bool hp10_sky_mask_mean(const uint8_t *jpeg, size_t jpeg_len,
                        double *r, double *g, double *b);

esp_err_t hp10_sky_compute(const uint8_t *jpeg, size_t jpeg_len,
                           uint16_t frame_w, uint16_t frame_h,
                           char *json_out, size_t json_out_n);
bool hp10_sky_latest(char *dst, size_t n);
