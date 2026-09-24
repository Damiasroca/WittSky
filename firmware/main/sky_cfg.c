#include "esp_log.h"

#include <stdbool.h>

#include "sky_awb.h"
#include "sky_cfg.h"

static const char *TAG = "sky";

uint8_t  g_sky_en;
uint8_t  g_sky_x;
uint8_t  g_sky_y;
uint8_t  g_sky_w = 100;
uint8_t  g_sky_h = 50;
uint16_t g_sky_rb = 600;
uint8_t  g_sky_sat = 250;
uint8_t  g_sky_awb;
uint16_t g_sky_daym = 30;

static bool in_range(int v, int lo, int hi)
{
    return v >= lo && v <= hi;
}

const char *hp10_sky_cfg_apply(int en, int x, int y, int w, int h,
                               int rb, int sat, int awb, int daym)
{
    if (!in_range(en, 0, 1))
        return "sky_en must be 0 or 1";
    if (!in_range(x, 0, 100))
        return "sky_x must be 0-100";
    if (!in_range(y, 0, 100))
        return "sky_y must be 0-100";
    if (!in_range(w, 1, 100))
        return "sky_w must be 1-100";
    if (!in_range(h, 1, 100))
        return "sky_h must be 1-100";
    if (!in_range(rb, 200, 2000))
        return "sky_rb must be 200-2000";
    if (!in_range(sat, 200, 255))
        return "sky_sat must be 200-255";
    if (!in_range(awb, 0, 4))
        return "sky_awb must be 0-4";
    if (!in_range(daym, 0, 180))
        return "sky_daym must be 0-180";

    uint8_t prev_awb = g_sky_awb;
    g_sky_en = (uint8_t)en;
    g_sky_x = (uint8_t)x;
    g_sky_y = (uint8_t)y;
    g_sky_w = (uint8_t)w;
    g_sky_h = (uint8_t)h;
    g_sky_rb = (uint16_t)rb;
    g_sky_sat = (uint8_t)sat;
    g_sky_awb = (uint8_t)awb;
    g_sky_daym = (uint16_t)daym;
    if (g_sky_awb != prev_awb)
        hp10_sky_awb_apply_locked();
    return NULL;
}

static void load_u8(nvs_handle_t h, const char *key, uint8_t *dst, int lo, int hi)
{
    uint8_t v;
    if (nvs_get_u8(h, key, &v) != ESP_OK)
        return;
    if (in_range(v, lo, hi))
        *dst = v;
    else
        ESP_LOGW(TAG, "nvs %s=%u out of range", key, v);
}

static void load_u16(nvs_handle_t h, const char *key, uint16_t *dst, int lo, int hi)
{
    uint16_t v;
    if (nvs_get_u16(h, key, &v) != ESP_OK)
        return;
    if (in_range(v, lo, hi))
        *dst = v;
    else
        ESP_LOGW(TAG, "nvs %s=%u out of range", key, v);
}

void hp10_sky_cfg_load(nvs_handle_t h)
{
    load_u8(h, "sky_en", &g_sky_en, 0, 1);
    load_u8(h, "sky_x", &g_sky_x, 0, 100);
    load_u8(h, "sky_y", &g_sky_y, 0, 100);
    load_u8(h, "sky_w", &g_sky_w, 1, 100);
    load_u8(h, "sky_h", &g_sky_h, 1, 100);
    load_u16(h, "sky_rb", &g_sky_rb, 200, 2000);
    load_u8(h, "sky_sat", &g_sky_sat, 200, 255);
    load_u8(h, "sky_awb", &g_sky_awb, 0, 4);
    load_u16(h, "sky_daym", &g_sky_daym, 0, 180);
}

void hp10_sky_cfg_save(nvs_handle_t h)
{
    nvs_set_u8(h, "sky_en", g_sky_en);
    nvs_set_u8(h, "sky_x", g_sky_x);
    nvs_set_u8(h, "sky_y", g_sky_y);
    nvs_set_u8(h, "sky_w", g_sky_w);
    nvs_set_u8(h, "sky_h", g_sky_h);
    nvs_set_u16(h, "sky_rb", g_sky_rb);
    nvs_set_u8(h, "sky_sat", g_sky_sat);
    nvs_set_u8(h, "sky_awb", g_sky_awb);
    nvs_set_u16(h, "sky_daym", g_sky_daym);
}
