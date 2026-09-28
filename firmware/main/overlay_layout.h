#pragma once

#include "overlay_draw.h"
#include "overlay_wx.h"

#include <stdint.h>

enum {
    OV_EL_TS = 0,
    OV_EL_WIND,
    OV_EL_TEMP,
    OV_EL_RAIN,
    OV_EL_ROSE,
    OV_EL_NEEDLE,
    OV_EL_N
};

enum {
    OV_AN_TL = 0,
    OV_AN_TC,
    OV_AN_TR,
    OV_AN_CL,
    OV_AN_CR,
    OV_AN_BL,
    OV_AN_BC,
    OV_AN_BR,
    OV_AN_N
};

/* Offsets are thousandths of the frame, added to the anchor point.
 * For every element except the needle, the anchor point is that corner
 * or edge of the element's box. The needle's anchor point is its center.
 * Wind unit: 0 km/h, 1 m/s, 2 mph, 3 kn. Temperature unit: 0 C, 1 F.
 */
typedef struct {
    uint8_t en[OV_EL_N];
    uint8_t an[OV_EL_N];
    int16_t ox[OV_EL_N];
    int16_t oy[OV_EL_N];
    uint8_t wind_unit;
    uint8_t temp_unit;
} ov_geom_t;

typedef struct ov_layout ov_layout_t;

void ov_geom_defaults(ov_geom_t *g);
int ov_layout_build(ov_layout_t **out, const ov_geom_t *g, int fw, int fh,
                    const ov_sample_t *wx, const char *timestamp);
void ov_layout_draw(const ov_layout_t *L, ov_band_t *band);
void ov_layout_free(ov_layout_t *L);
