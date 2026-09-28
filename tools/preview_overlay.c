#define OV_HOST 1

#include "overlay_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_px(uint8_t *rgb, int w, int x, int y, int r, int g, int b)
{
    uint8_t *p = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3;
    p[0] = (uint8_t)r;
    p[1] = (uint8_t)g;
    p[2] = (uint8_t)b;
}

static void sky(uint8_t *rgb, int w, int h)
{
    for (int y = 0; y < h; y++) {
        int b = 170 + y * 40 / h;
        for (int x = 0; x < w; x++)
            put_px(rgb, w, x, y, 150, 175, b > 255 ? 255 : b);
    }
}

static int write_ppm(const char *path, const uint8_t *rgb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    size_t n = (size_t)w * (size_t)h * 3;
    if (fwrite(rgb, 1, n, f) != n) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int render(const char *path, int w, int h, int raining)
{
    ov_geom_t g;
    ov_layout_t *L = NULL;
    ov_sample_t wx;
    uint8_t *rgb;
    int rc;

    ov_geom_defaults(&g);
    memset(&wx, 0, sizeof wx);
    wx.ok = 1;
    wx.wind_mms = 6389;
    wx.temp_mc = 27200;
    wx.dir_deg = 180;
    wx.raining = raining;
    rgb = malloc((size_t)w * (size_t)h * 3);
    if (!rgb)
        return -1;
    sky(rgb, w, h);
    rc = ov_layout_build(&L, &g, w, h, &wx, "2026-09-28 15:04:12");
    if (rc != 0) {
        free(rgb);
        return -1;
    }
    ov_band_t band = {.rgb = rgb, .frame_w = w, .y0 = 0, .rows = h};
    ov_layout_draw(L, &band);
    ov_layout_free(L);
    rc = write_ppm(path, rgb, w, h);
    free(rgb);
    return rc;
}

int main(void)
{
    if (render("overlay_preview_uxga.ppm", 1600, 1200, 1) != 0)
        return 1;
    if (render("overlay_preview_svga.ppm", 800, 600, 0) != 0)
        return 1;
    printf("wrote previews\n");
    return 0;
}
