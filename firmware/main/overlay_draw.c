#include "overlay_draw.h"

#include "overlay_assets.h"

#include <math.h>
#include <string.h>

#include "miniz.h"

static void blend(uint8_t *p, int r, int g, int b, int a)
{
    if (a <= 0)
        return;
    if (a >= 255) {
        p[0] = (uint8_t)r;
        p[1] = (uint8_t)g;
        p[2] = (uint8_t)b;
        return;
    }
    p[0] = (uint8_t)((r * a + p[0] * (255 - a) + 127) / 255);
    p[1] = (uint8_t)((g * a + p[1] * (255 - a) + 127) / 255);
    p[2] = (uint8_t)((b * a + p[2] * (255 - a) + 127) / 255);
}

static uint8_t *at(ov_band_t *b, int x, int y)
{
    if (!b->rgb || x < 0 || y < b->y0 || y >= b->y0 + b->rows)
        return NULL;
    if ((unsigned)x >= (unsigned)b->frame_w)
        return NULL;
    return b->rgb + ((size_t)(y - b->y0) * (size_t)b->frame_w + (size_t)x) * 3;
}

void ov_fill_rect(ov_band_t *b, int x, int y, int w, int h,
                  int r, int g, int bl, int a)
{
    int x1, y1;
    if (w <= 0 || h <= 0 || a <= 0)
        return;
    x1 = x + w;
    y1 = y + h;
    if (x < 0)
        x = 0;
    if (y < b->y0)
        y = b->y0;
    if (x1 > b->frame_w)
        x1 = b->frame_w;
    if (y1 > b->y0 + b->rows)
        y1 = b->y0 + b->rows;
    for (int yy = y; yy < y1; yy++) {
        uint8_t *row = at(b, x, yy);
        if (!row)
            continue;
        for (int xx = x; xx < x1; xx++, row += 3)
            blend(row, r, g, bl, a);
    }
}

static const ov_glyph_t *glyph(const ov_font_t *font, unsigned cp)
{
    for (uint16_t i = 0; i < font->n; i++) {
        if (font->glyphs[i].cp == cp)
            return &font->glyphs[i];
    }
    return NULL;
}

static unsigned next_cp(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    if (!s || !*s)
        return 0;
    if (s[0] < 0x80) {
        *p += 1;
        return s[0];
    }
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *p += 2;
        return ((unsigned)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    }
    *p += 1;
    return 0;
}

int ov_text_width(const ov_font_t *font, const char *utf8)
{
    int w = 0;
    const char *p = utf8 ? utf8 : "";
    while (*p) {
        unsigned cp = next_cp(&p);
        const ov_glyph_t *g;
        if (!cp)
            continue;
        g = glyph(font, cp);
        w += g ? g->adv : font->px / 2;
    }
    return w;
}

const ov_font_t *ov_font_pick(int frame_h)
{
    int target = 30 * frame_h / 1200;
    const ov_font_t *best = &ov_fonts[0];
    int best_d = 100000;
    if (target < 1)
        target = 1;
    for (int i = 0; i < ov_font_n; i++) {
        int d = (int)ov_fonts[i].px - target;
        if (d < 0)
            d = -d;
        if (d < best_d) {
            best_d = d;
            best = &ov_fonts[i];
        }
    }
    return best;
}

void ov_draw_text(ov_band_t *b, const ov_font_t *font, int x, int y,
                  const char *utf8)
{
    const char *p = utf8 ? utf8 : "";
    int pen = x;
    while (*p) {
        unsigned cp = next_cp(&p);
        const ov_glyph_t *g;
        const uint8_t *bits;
        if (!cp)
            continue;
        g = glyph(font, cp);
        if (!g) {
            pen += font->px / 2;
            continue;
        }
        bits = font->bits + g->off;
        for (int row = 0; row < g->h; row++) {
            int yy = y + g->yoff + row;
            for (int col = 0; col < g->w; col++) {
                int i = row * ((g->w + 1) / 2) + col / 2;
                int nib = (col & 1) ? (bits[i] & 15) : (bits[i] >> 4);
                uint8_t *px;
                if (!nib)
                    continue;
                px = at(b, pen + g->xoff + col, yy);
                if (px)
                    blend(px, 255, 255, 255, nib * 17);
            }
        }
        pen += g->adv;
    }
}

static int sample_u8(const uint8_t *s, int w, int h, int x_fp, int y_fp)
{
    int x = x_fp >> 8;
    int y = y_fp >> 8;
    int fx = x_fp & 255;
    int fy = y_fp & 255;
    int x1, y1, a00, a10, a01, a11, a0, a1;
    if (w <= 0 || h <= 0)
        return 0;
    if (x < 0) {
        x = 0;
        fx = 0;
    }
    if (y < 0) {
        y = 0;
        fy = 0;
    }
    if (x >= w) {
        x = w - 1;
        fx = 0;
    }
    if (y >= h) {
        y = h - 1;
        fy = 0;
    }
    x1 = x + 1 < w ? x + 1 : x;
    y1 = y + 1 < h ? y + 1 : y;
    a00 = s[y * w + x];
    a10 = s[y * w + x1];
    a01 = s[y1 * w + x];
    a11 = s[y1 * w + x1];
    a0 = a00 + ((a10 - a00) * fx) / 256;
    a1 = a01 + ((a11 - a01) * fx) / 256;
    return a0 + ((a1 - a0) * fy) / 256;
}

void ov_blit_alpha(ov_band_t *b, const uint8_t *alpha, int src_w, int src_h,
                   int dx, int dy, int dw, int dh,
                   int r, int g, int bl)
{
    int x0, y0, x1, y1;
    if (!alpha || dw <= 0 || dh <= 0 || src_w <= 0 || src_h <= 0)
        return;
    x0 = dx;
    y0 = dy;
    x1 = dx + dw;
    y1 = dy + dh;
    if (x0 < 0)
        x0 = 0;
    if (y0 < b->y0)
        y0 = b->y0;
    if (x1 > b->frame_w)
        x1 = b->frame_w;
    if (y1 > b->y0 + b->rows)
        y1 = b->y0 + b->rows;
    for (int y = y0; y < y1; y++) {
        int sy = (int)(((int64_t)(y - dy) * src_h << 8) / dh);
        for (int x = x0; x < x1; x++) {
            int sx = (int)(((int64_t)(x - dx) * src_w << 8) / dw);
            int a = sample_u8(alpha, src_w, src_h, sx, sy);
            uint8_t *px;
            if (a <= 0)
                continue;
            px = at(b, x, y);
            if (px)
                blend(px, r, g, bl, a);
        }
    }
}

static void rgb565_to(uint16_t c, int *r, int *g, int *b)
{
    *r = ((c >> 11) & 31) * 255 / 31;
    *g = ((c >> 5) & 63) * 255 / 63;
    *b = (c & 31) * 255 / 31;
}

static int rain_a(int i)
{
    const uint8_t *a4 = ov_rain_asset.a4;
    int nib = (i & 1) ? (a4[i >> 1] & 15) : (a4[i >> 1] >> 4);
    return nib * 17;
}

void ov_blit_rain(ov_band_t *b, int dx, int dy, int dw, int dh)
{
    int sw = ov_rain_asset.w;
    int sh = ov_rain_asset.h;
    int x0, y0, x1, y1;
    if (dw <= 0 || dh <= 0)
        return;
    x0 = dx < 0 ? 0 : dx;
    y0 = dy < b->y0 ? b->y0 : dy;
    x1 = dx + dw;
    y1 = dy + dh;
    if (x1 > b->frame_w)
        x1 = b->frame_w;
    if (y1 > b->y0 + b->rows)
        y1 = b->y0 + b->rows;
    for (int y = y0; y < y1; y++) {
        int sy = (int)(((int64_t)(y - dy) * sh << 8) / dh);
        int iy = sy >> 8;
        int fy = sy & 255;
        if (iy < 0)
            iy = 0;
        if (iy >= sh) {
            iy = sh - 1;
            fy = 0;
        }
        int iy1 = iy + 1 < sh ? iy + 1 : iy;
        for (int x = x0; x < x1; x++) {
            int sx = (int)(((int64_t)(x - dx) * sw << 8) / dw);
            int ix = sx >> 8;
            int fx = sx & 255;
            int ix1, a00, a10, a01, a11, a;
            int r00, g00, b00, r10, g10, b10, r01, g01, b01, r11, g11, b11;
            int r0, g0, b0, r1, g1, b1, rr, gg, bb;
            uint8_t *px;
            if (ix < 0) {
                ix = 0;
                fx = 0;
            }
            if (ix >= sw) {
                ix = sw - 1;
                fx = 0;
            }
            ix1 = ix + 1 < sw ? ix + 1 : ix;
            a00 = rain_a(iy * sw + ix);
            a10 = rain_a(iy * sw + ix1);
            a01 = rain_a(iy1 * sw + ix);
            a11 = rain_a(iy1 * sw + ix1);
            a = a00 + ((a10 - a00) * fx) / 256;
            a = a + (((a01 + ((a11 - a01) * fx) / 256) - a) * fy) / 256;
            if (a <= 0)
                continue;
            rgb565_to(ov_rain_asset.rgb565[iy * sw + ix], &r00, &g00, &b00);
            rgb565_to(ov_rain_asset.rgb565[iy * sw + ix1], &r10, &g10, &b10);
            rgb565_to(ov_rain_asset.rgb565[iy1 * sw + ix], &r01, &g01, &b01);
            rgb565_to(ov_rain_asset.rgb565[iy1 * sw + ix1], &r11, &g11, &b11);
            r0 = r00 + ((r10 - r00) * fx) / 256;
            g0 = g00 + ((g10 - g00) * fx) / 256;
            b0 = b00 + ((b10 - b00) * fx) / 256;
            r1 = r01 + ((r11 - r01) * fx) / 256;
            g1 = g01 + ((g11 - g01) * fx) / 256;
            b1 = b01 + ((b11 - b01) * fx) / 256;
            rr = r0 + ((r1 - r0) * fy) / 256;
            gg = g0 + ((g1 - g0) * fy) / 256;
            bb = b0 + ((b1 - b0) * fy) / 256;
            px = at(b, x, y);
            if (px)
                blend(px, rr, gg, bb, a);
        }
    }
}

static int cross(int ax, int ay, int bx, int by, int px, int py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

static int inside(const int p[4][2], int x, int y)
{
    int sign = 0;
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) & 3;
        int c = cross(p[i][0], p[i][1], p[j][0], p[j][1], x, y);
        if (c == 0)
            continue;
        if (!sign)
            sign = c > 0 ? 1 : -1;
        else if ((c > 0) != (sign > 0))
            return 0;
    }
    return 1;
}

static void rot_poly(float dst[4][2], float cx, float cy, float ang, float scale,
                     const float local[4][2])
{
    float c = cosf(ang);
    float s = sinf(ang);
    for (int i = 0; i < 4; i++) {
        float lx = local[i][0] * scale;
        float ly = local[i][1] * scale;
        dst[i][0] = cx + lx * c - ly * s;
        dst[i][1] = cy + lx * s + ly * c;
    }
}

static void expand_poly(float dst[4][2], const float src[4][2], float cx, float cy,
                        float px)
{
    for (int i = 0; i < 4; i++) {
        float dx = src[i][0] - cx;
        float dy = src[i][1] - cy;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 1.f)
            len = 1.f;
        dst[i][0] = src[i][0] + dx / len * px;
        dst[i][1] = src[i][1] + dy / len * px;
    }
}

static void to_q(int q[4][2], const float p[4][2])
{
    for (int i = 0; i < 4; i++) {
        q[i][0] = (int)lroundf(p[i][0] * 4.f);
        q[i][1] = (int)lroundf(p[i][1] * 4.f);
    }
}

static void wind_arrow_color(int32_t wind_mms, int *r, int *g, int *b)
{
    static const int stops[5][3] = {
        { 32, 96, 220 },
        { 30, 170, 55 },
        { 235, 200, 20 },
        { 240, 120, 20 },
        { 210, 30, 30 },
    };
    const int32_t full = 19445; /* 70 km/h in mm/s */
    int seg;
    int t;
    int32_t span;
    int32_t into;
    if (wind_mms < 0)
        wind_mms = 0;
    if (wind_mms >= full) {
        *r = stops[4][0];
        *g = stops[4][1];
        *b = stops[4][2];
        return;
    }
    span = wind_mms * 4;
    seg = (int)(span / full);
    if (seg > 3)
        seg = 3;
    into = span - (int32_t)seg * full;
    t = (int)(into * 256 / full);
    if (t > 255)
        t = 255;
    *r = stops[seg][0] + (stops[seg + 1][0] - stops[seg][0]) * t / 256;
    *g = stops[seg][1] + (stops[seg + 1][1] - stops[seg][1]) * t / 256;
    *b = stops[seg][2] + (stops[seg + 1][2] - stops[seg][2]) * t / 256;
}

void ov_draw_needle(ov_band_t *b, int cx, int cy, int dir_deg, int radius,
                    int32_t wind_mms)
{
    static const float local[4][2] = {
        { 0.f, -1.00f },
        { 0.07f, -0.86f },
        { 0.f, -0.80f },
        { -0.07f, -0.86f },
    };
    float mark[4][2], dark[4][2];
    int cr, cg, cb;
    int qy[4][2], qd[4][2];
    int minx, miny, maxx, maxy;
    int deg = dir_deg % 360;
    float ang;
    if (radius < 6)
        return;
    if (deg < 0)
        deg += 360;
    ang = (float)deg * 0.0174532925f;
    wind_arrow_color(wind_mms, &cr, &cg, &cb);
    rot_poly(mark, (float)cx, (float)cy, ang, (float)radius, local);
    expand_poly(dark, mark, (float)cx, (float)cy, 1.7f);
    to_q(qy, mark);
    to_q(qd, dark);
    minx = maxx = (int)lroundf(dark[0][0]);
    miny = maxy = (int)lroundf(dark[0][1]);
    for (int i = 1; i < 4; i++) {
        int x = (int)lroundf(dark[i][0]);
        int y = (int)lroundf(dark[i][1]);
        if (x < minx)
            minx = x;
        if (x > maxx)
            maxx = x;
        if (y < miny)
            miny = y;
        if (y > maxy)
            maxy = y;
    }
    minx -= 1;
    miny -= 1;
    maxx += 1;
    maxy += 1;
    if (minx < 0)
        minx = 0;
    if (miny < b->y0)
        miny = b->y0;
    if (maxx >= b->frame_w)
        maxx = b->frame_w - 1;
    if (maxy >= b->y0 + b->rows)
        maxy = b->y0 + b->rows - 1;
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            int ny = 0;
            int nd = 0;
            uint8_t *px;
            for (int sy = 0; sy < 2; sy++) {
                for (int sx = 0; sx < 2; sx++) {
                    int qx = x * 4 + 1 + sx * 2;
                    int qyy = y * 4 + 1 + sy * 2;
                    if (inside(qy, qx, qyy))
                        ny++;
                    else if (inside(qd, qx, qyy))
                        nd++;
                }
            }
            if (!ny && !nd)
                continue;
            px = at(b, x, y);
            if (!px)
                continue;
            if (ny)
                blend(px, cr, cg, cb, ny * 255 / 4);
            else
                blend(px, 24, 24, 24, nd * 255 / 4);
        }
    }
}

int ov_rose_inflate(uint8_t *dst, size_t dst_n)
{
    if (!dst || dst_n < ov_rose_asset.alpha_len)
        return -1;
#if defined(OV_HOST)
    uLongf n = (uLongf)dst_n;
    if (uncompress(dst, &n, ov_rose_asset.z, ov_rose_asset.zlen) != Z_OK)
        return -1;
    return n == ov_rose_asset.alpha_len ? 0 : -1;
#else
    size_t n = tinfl_decompress_mem_to_mem(
        dst, dst_n, ov_rose_asset.z, ov_rose_asset.zlen,
        TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    return n == ov_rose_asset.alpha_len ? 0 : -1;
#endif
}
