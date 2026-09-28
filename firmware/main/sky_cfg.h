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
extern uint8_t  g_sky_awb_r;
extern uint8_t  g_sky_awb_g;
extern uint8_t  g_sky_awb_b;
extern uint16_t g_sky_daym;

#define SKY_AWB_PRESET0  5
#define SKY_AWB_PRESET_N 4
#define SKY_AWB_PRESET_LAST (SKY_AWB_PRESET0 + SKY_AWB_PRESET_N - 1)
#define SKY_AWB_NAME_MAX 15

typedef struct {
    uint8_t used;
    uint8_t r, g, b;
    char name[SKY_AWB_NAME_MAX + 1];
} sky_awb_preset_t;

extern sky_awb_preset_t g_sky_preset[SKY_AWB_PRESET_N];

const char *hp10_sky_awb_name_ok(const char *name);
const char *hp10_sky_awb_set(int awb, int r, int g, int b);
const char *hp10_sky_awb_preset_save(const char *name, int r, int g, int b);
const char *hp10_sky_awb_preset_delete(int slot);
const char *hp10_sky_cfg_apply(int en, int x, int y, int w, int h,
                               int rb, int sat, int awb,
                               int awb_r, int awb_g, int awb_b, int daym);
void hp10_sky_cfg_load(nvs_handle_t h);
void hp10_sky_cfg_save(nvs_handle_t h);
