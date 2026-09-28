#include "overlay_layout.h"

#include "overlay_assets.h"
#include "overlay_units.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(OV_HOST)
#include "esp_heap_caps.h"
#endif

typedef struct {
    int on;
    int x, y, w, h;
    int text_x, text_y;
    char text[32];
    const ov_font_t *font;
} ov_label_t;

struct ov_layout {
    ov_label_t ts, wind, temp;
    int rain_on, rose_on, needle_on;
    int rain_x, rain_y, rain_w, rain_h;
    int rose_x, rose_y, rose_w, rose_h;
    int red_x, red_y, red_w, red_h;
    int ncx, ncy, nr, ndeg;
    int32_t wind_mms;
    uint8_t *rose_a;
};

static void *ov_alloc(size_t n)
{
#if defined(OV_HOST)
    return malloc(n);
#else
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p)
        p = malloc(n);
    return p;
#endif
}

void ov_geom_defaults(ov_geom_t *g)
{
    memset(g, 0, sizeof *g);
    for (int i = 0; i < OV_EL_N; i++)
        g->en[i] = 1;
    g->an[OV_EL_TS] = OV_AN_TL;
    g->ox[OV_EL_TS] = 12;
    g->oy[OV_EL_TS] = 10;
    g->an[OV_EL_WIND] = OV_AN_TR;
    g->ox[OV_EL_WIND] = -12;
    g->oy[OV_EL_WIND] = 10;
    g->an[OV_EL_TEMP] = OV_AN_TR;
    g->ox[OV_EL_TEMP] = -12;
    g->oy[OV_EL_TEMP] = 56;
    g->an[OV_EL_RAIN] = OV_AN_TR;
    g->ox[OV_EL_RAIN] = -12;
    g->oy[OV_EL_RAIN] = 102;
    g->an[OV_EL_ROSE] = OV_AN_BL;
    g->ox[OV_EL_ROSE] = 12;
    g->oy[OV_EL_ROSE] = -12;
    /* Center of the default rose on a 4:3 frame. */
    g->an[OV_EL_NEEDLE] = OV_AN_BL;
    g->ox[OV_EL_NEEDLE] = 147;
    g->oy[OV_EL_NEEDLE] = -192;
    g->wind_unit = 0;
    g->temp_unit = 0;
}

static void anchor_xy(int an, int fw, int fh, int ox, int oy, int *x, int *y)
{
    int ax = 0;
    int ay = 0;
    if (an == OV_AN_TC || an == OV_AN_BC)
        ax = fw / 2;
    else if (an == OV_AN_TR || an == OV_AN_CR || an == OV_AN_BR)
        ax = fw;
    if (an == OV_AN_CL || an == OV_AN_CR)
        ay = fh / 2;
    else if (an == OV_AN_BL || an == OV_AN_BC || an == OV_AN_BR)
        ay = fh;
    *x = ax + ox * fw / 1000;
    *y = ay + oy * fh / 1000;
}

static void pin_box(int an, int ax, int ay, int bw, int bh, int *x, int *y)
{
    if (an == OV_AN_TR || an == OV_AN_CR || an == OV_AN_BR)
        ax -= bw;
    else if (an == OV_AN_TC || an == OV_AN_BC)
        ax -= bw / 2;
    if (an == OV_AN_BL || an == OV_AN_BC || an == OV_AN_BR)
        ay -= bh;
    else if (an == OV_AN_CL || an == OV_AN_CR)
        ay -= bh / 2;
    *x = ax;
    *y = ay;
}

static void place_label(ov_label_t *lab, int an, int ox, int oy, int fw, int fh,
                        const ov_font_t *font, const char *text)
{
    int ax, ay, pad_x, pad_y, tw;
    memset(lab, 0, sizeof *lab);
    if (!text || !text[0] || !font)
        return;
    pad_x = font->px / 3;
    pad_y = font->px / 6;
    if (pad_x < 2)
        pad_x = 2;
    if (pad_y < 1)
        pad_y = 1;
    tw = ov_text_width(font, text);
    lab->w = tw + pad_x * 2;
    lab->h = pad_y + font->ascent + font->descent + pad_y;
    anchor_xy(an, fw, fh, ox, oy, &ax, &ay);
    pin_box(an, ax, ay, lab->w, lab->h, &lab->x, &lab->y);
    lab->text_x = lab->x + pad_x;
    lab->text_y = lab->y + pad_y;
    lab->font = font;
    lab->on = 1;
    snprintf(lab->text, sizeof lab->text, "%s", text);
}

int ov_layout_build(ov_layout_t **out, const ov_geom_t *g, int fw, int fh,
                    const ov_sample_t *wx, const char *timestamp)
{
    ov_layout_t *L;
    const ov_font_t *font;
    char wind[32], temp[24];
    int side, ax, ay;

    *out = NULL;
    if (!g || fw < 16 || fh < 16)
        return -1;
    L = calloc(1, sizeof *L);
    if (!L)
        return -1;
    font = ov_font_pick(fh);
    if (g->en[OV_EL_TS])
        place_label(&L->ts, g->an[OV_EL_TS], g->ox[OV_EL_TS], g->oy[OV_EL_TS],
                    fw, fh, font, timestamp);
    if (wx && wx->ok && g->en[OV_EL_WIND]) {
        ov_fmt_wind(wind, sizeof wind, wx->wind_mms, wx->dir_deg, g->wind_unit);
        place_label(&L->wind, g->an[OV_EL_WIND], g->ox[OV_EL_WIND], g->oy[OV_EL_WIND],
                    fw, fh, font, wind);
    }
    if (wx && wx->ok && g->en[OV_EL_TEMP]) {
        ov_fmt_temp(temp, sizeof temp, wx->temp_mc, g->temp_unit);
        place_label(&L->temp, g->an[OV_EL_TEMP], g->ox[OV_EL_TEMP], g->oy[OV_EL_TEMP],
                    fw, fh, font, temp);
    }
    if (wx && wx->ok && wx->raining && g->en[OV_EL_RAIN]) {
        int rw = fh * 78 / 1200;
        if (rw < 8)
            rw = 8;
        anchor_xy(g->an[OV_EL_RAIN], fw, fh, g->ox[OV_EL_RAIN], g->oy[OV_EL_RAIN],
                  &ax, &ay);
        pin_box(g->an[OV_EL_RAIN], ax, ay, rw, rw, &L->rain_x, &L->rain_y);
        L->rain_w = rw;
        L->rain_h = rw;
        L->rain_on = 1;
    }
    side = fh * 432 / 1200;
    if (g->en[OV_EL_ROSE] && side >= 16) {
        anchor_xy(g->an[OV_EL_ROSE], fw, fh, g->ox[OV_EL_ROSE], g->oy[OV_EL_ROSE],
                  &ax, &ay);
        pin_box(g->an[OV_EL_ROSE], ax, ay, side, side, &L->rose_x, &L->rose_y);
        L->rose_w = side;
        L->rose_h = side;
        L->rose_a = ov_alloc(ov_rose_asset.alpha_len);
        if (!L->rose_a || ov_rose_inflate(L->rose_a, ov_rose_asset.alpha_len) != 0) {
            ov_layout_free(L);
            return -1;
        }
        if (ov_rose_asset.red_w && ov_rose_asset.red_h) {
            L->red_w = side * ov_rose_asset.red_w / ov_rose_asset.w;
            L->red_h = side * ov_rose_asset.red_h / ov_rose_asset.h;
            if (L->red_w < 1)
                L->red_w = 1;
            if (L->red_h < 1)
                L->red_h = 1;
            L->red_x = L->rose_x + side * ov_rose_asset.red_x / ov_rose_asset.w;
            L->red_y = L->rose_y + side * ov_rose_asset.red_y / ov_rose_asset.h;
        }
        L->rose_on = 1;
    }
    if (wx && wx->ok && g->en[OV_EL_NEEDLE]) {
        if (L->rose_on) {
            L->ncx = L->rose_x + side / 2;
            L->ncy = L->rose_y + side / 2;
            /* Outer end of the degree ticks on rosa4.png. */
            L->nr = side * 36 / 100;
        } else {
            anchor_xy(g->an[OV_EL_NEEDLE], fw, fh, g->ox[OV_EL_NEEDLE], g->oy[OV_EL_NEEDLE],
                      &L->ncx, &L->ncy);
            L->nr = fh * 162 / 1000;
        }
        L->ndeg = wx->dir_deg;
        L->wind_mms = wx->wind_mms;
        L->needle_on = 1;
    }
    *out = L;
    return 0;
}

void ov_layout_draw(const ov_layout_t *L, ov_band_t *band)
{
    if (!L || !band)
        return;
    if (L->rose_on) {
        ov_blit_alpha(band, L->rose_a, ov_rose_asset.w, ov_rose_asset.h,
                      L->rose_x, L->rose_y, L->rose_w, L->rose_h,
                      200, 200, 200);
        if (L->red_w)
            ov_blit_alpha(band, ov_rose_asset.red_a, ov_rose_asset.red_w,
                          ov_rose_asset.red_h, L->red_x, L->red_y, L->red_w, L->red_h,
                          220, 16, 16);
    }
    if (L->needle_on)
        ov_draw_needle(band, L->ncx, L->ncy, L->ndeg, L->nr, L->wind_mms);
    if (L->rain_on)
        ov_blit_rain(band, L->rain_x, L->rain_y, L->rain_w, L->rain_h);
    const ov_label_t *labs[3] = { &L->ts, &L->wind, &L->temp };
    for (int i = 0; i < 3; i++) {
        const ov_label_t *lab = labs[i];
        if (!lab->on)
            continue;
        ov_fill_rect(band, lab->x, lab->y, lab->w, lab->h, 0, 0, 0, 160);
        ov_draw_text(band, lab->font, lab->text_x, lab->text_y, lab->text);
    }
}

void ov_layout_free(ov_layout_t *L)
{
    if (!L)
        return;
    free(L->rose_a);
    free(L);
}
