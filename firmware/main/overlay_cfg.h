#pragma once

#include "nvs.h"
#include "overlay_layout.h"

#include <stdint.h>

extern uint8_t  g_ov_en;
extern uint8_t  g_ov_prov;    /* OV_PROV_* */
extern uint8_t  g_ov_sub;     /* OV_SUB_LOCAL or OV_SUB_CLOUD */
extern uint8_t  g_ov_clean;   /* Ecowitt upload keeps the original JPEG */
extern uint32_t g_ov_cache_ms;
extern uint16_t g_ov_timeout; /* milliseconds */
extern wx_cfg_t g_wx;
extern ov_geom_t g_ov_geom;

typedef struct {
    int en, prov, sub, clean, port, timeout, cache_ms;
    int wind_unit, temp_unit, press_unit, rain_unit;
    const char *host;
    const char *app_key;
    const char *api_key;
    const char *api_secret;
    const char *token;
    const char *station_id;
    uint8_t en_el[OV_EL_N];
    uint8_t an_el[OV_EL_N];
    int ox[OV_EL_N];
    int oy[OV_EL_N];
    uint8_t slot_meas[OV_SLOT_N];
} ov_cfg_in_t;

const char *hp10_overlay_cfg_apply(const ov_cfg_in_t *in);
void hp10_overlay_cfg_load(nvs_handle_t h);
void hp10_overlay_cfg_save(nvs_handle_t h);

/* Snapshot the fetch-relevant config under a short lock. The caller passes
 * stable values to wx_fetch while apply() runs on the HTTP server task.
 */
void hp10_overlay_cfg_snapshot(wx_cfg_t *wx, uint8_t *prov, uint8_t *sub,
                               uint16_t *timeout, uint32_t *cache_ms);
