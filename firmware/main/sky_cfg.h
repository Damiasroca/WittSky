#pragma once

#include "nvs.h"

#include <stdint.h>

extern uint8_t  g_sky_en;
extern uint8_t  g_sky_x;
extern uint8_t  g_sky_y;
extern uint8_t  g_sky_w;
extern uint8_t  g_sky_h;
extern uint16_t g_sky_rb;
extern uint8_t  g_sky_sat;
extern uint8_t  g_sky_awb;
extern uint16_t g_sky_daym;

const char *hp10_sky_cfg_apply(int en, int x, int y, int w, int h,
                               int rb, int sat, int awb, int daym);
void hp10_sky_cfg_load(nvs_handle_t h);
void hp10_sky_cfg_save(nvs_handle_t h);
