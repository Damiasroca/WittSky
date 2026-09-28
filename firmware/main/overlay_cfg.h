#pragma once

#include "nvs.h"
#include "overlay_layout.h"

#include <stdint.h>

extern uint8_t  g_ov_en;
extern uint8_t  g_ov_sta;     /* 0 none, 1 local gateway, 2 Ecowitt cloud */
extern uint8_t  g_ov_clean;   /* Ecowitt upload keeps the original JPEG */
extern uint16_t g_ov_port;
extern uint16_t g_ov_timeout; /* milliseconds */
extern char     g_ov_host[64];
extern char     g_ov_url[384];
extern ov_geom_t g_ov_geom;

const char *hp10_overlay_cfg_apply(int en, int sta, int clean, int port, int timeout,
                                   int wind_unit, int temp_unit, const char *host,
                                   const char *url,
                                   const uint8_t en_el[OV_EL_N],
                                   const uint8_t an_el[OV_EL_N],
                                   const int ox[OV_EL_N], const int oy[OV_EL_N]);
void hp10_overlay_cfg_load(nvs_handle_t h);
void hp10_overlay_cfg_save(nvs_handle_t h);
