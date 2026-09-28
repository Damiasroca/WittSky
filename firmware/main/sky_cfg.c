#include "esp_log.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

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
uint8_t  g_sky_awb_r = 0x5E;
uint8_t  g_sky_awb_g = 0x41;
uint8_t  g_sky_awb_b = 0x54;
sky_awb_preset_t g_sky_preset[SKY_AWB_PRESET_N];
uint16_t g_sky_daym = 30;

static bool in_range(int v, int lo, int hi)
{
    return v >= lo && v <= hi;
}

static int preset_slot(int awb)
{
    if (awb < SKY_AWB_PRESET0 || awb > SKY_AWB_PRESET_LAST)
        return -1;
    return awb - SKY_AWB_PRESET0;
}

static const char *awb_check(int awb, int r, int g, int b)
{
    int slot = preset_slot(awb);
    if (awb < 0 || (awb > 4 && slot < 0))
        return "sky_awb must be 0-4 or a saved preset";
    if (slot >= 0 && !g_sky_preset[slot].used)
        return "that preset is empty";
    if (!in_range(r, 0, 255) || !in_range(g, 0, 255) || !in_range(b, 0, 255))
        return "awb gain must be 0-255";
    return NULL;
}

static const char *copy_name(char *dst, const char *src)
{
    if (!src)
        return "Name the preset";
    while (*src == ' ')
        src++;
    size_t n = 0;
    while (src[n]) {
        unsigned char c = (unsigned char)src[n];
        if (c < 0x20)
            return "preset name has a bad character";
        if (n >= SKY_AWB_NAME_MAX)
            return "preset name must be 1-15 characters";
        dst[n] = (char)c;
        n++;
    }
    while (n > 0 && dst[n - 1] == ' ')
        n--;
    dst[n] = 0;
    if (n == 0)
        return "Name the preset";
    return NULL;
}

const char *hp10_sky_awb_name_ok(const char *name)
{
    char stored[SKY_AWB_NAME_MAX + 1];
    return copy_name(stored, name);
}

static void awb_store(int awb, int r, int g, int b)
{
    uint8_t prev_mode = g_sky_awb;
    g_sky_awb = (uint8_t)awb;
    g_sky_awb_r = (uint8_t)r;
    g_sky_awb_g = (uint8_t)g;
    g_sky_awb_b = (uint8_t)b;
    if (g_sky_awb != prev_mode)
        hp10_sky_awb_apply_locked();
}

const char *hp10_sky_awb_preset_save(const char *name, int r, int g, int b)
{
    if (!in_range(r, 0, 255) || !in_range(g, 0, 255) || !in_range(b, 0, 255))
        return "awb gain must be 0-255";
    int slot = -1;
    for (int i = 0; i < SKY_AWB_PRESET_N; i++) {
        if (!g_sky_preset[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return "No more presets can be saved.";
    char stored[SKY_AWB_NAME_MAX + 1];
    const char *why = copy_name(stored, name);
    if (why)
        return why;
    g_sky_preset[slot].used = 1;
    g_sky_preset[slot].r = (uint8_t)r;
    g_sky_preset[slot].g = (uint8_t)g;
    g_sky_preset[slot].b = (uint8_t)b;
    memcpy(g_sky_preset[slot].name, stored, sizeof stored);
    g_sky_awb = (uint8_t)(SKY_AWB_PRESET0 + slot);
    g_sky_awb_r = (uint8_t)r;
    g_sky_awb_g = (uint8_t)g;
    g_sky_awb_b = (uint8_t)b;
    hp10_sky_awb_apply_locked();
    return NULL;
}

const char *hp10_sky_awb_preset_delete(int slot)
{
    if (slot < 0 || slot >= SKY_AWB_PRESET_N || !g_sky_preset[slot].used)
        return "that preset is empty";
    memset(&g_sky_preset[slot], 0, sizeof g_sky_preset[slot]);
    if (g_sky_awb == (uint8_t)(SKY_AWB_PRESET0 + slot)) {
        g_sky_awb = 0;
        hp10_sky_awb_apply_locked();
    }
    return NULL;
}

const char *hp10_sky_awb_set(int awb, int r, int g, int b)
{
    const char *why = awb_check(awb, r, g, b);
    if (why)
        return why;
    awb_store(awb, r, g, b);
    return NULL;
}

const char *hp10_sky_cfg_apply(int en, int x, int y, int w, int h,
                               int rb, int sat, int awb,
                               int awb_r, int awb_g, int awb_b, int daym)
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
    const char *why = awb_check(awb, awb_r, awb_g, awb_b);
    if (why)
        return why;
    if (!in_range(daym, 0, 180))
        return "sky_daym must be 0-180";

    g_sky_en = (uint8_t)en;
    g_sky_x = (uint8_t)x;
    g_sky_y = (uint8_t)y;
    g_sky_w = (uint8_t)w;
    g_sky_h = (uint8_t)h;
    g_sky_rb = (uint16_t)rb;
    g_sky_sat = (uint8_t)sat;
    g_sky_daym = (uint16_t)daym;
    awb_store(awb, awb_r, awb_g, awb_b);
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
    load_u8(h, "sky_awb", &g_sky_awb, 0, SKY_AWB_PRESET_LAST);
    load_u8(h, "sky_awb_r", &g_sky_awb_r, 0, 255);
    load_u8(h, "sky_awb_g", &g_sky_awb_g, 0, 255);
    load_u8(h, "sky_awb_b", &g_sky_awb_b, 0, 255);
    load_u16(h, "sky_daym", &g_sky_daym, 0, 180);

    uint8_t present = 0;
    bool legacy = nvs_get_u8(h, "sky_p0u", &present) == ESP_ERR_NVS_NOT_FOUND;
    for (int i = 0; i < SKY_AWB_PRESET_N; i++) {
        char ku[8], kr[8], kg[8], kb[8], kn[8];
        snprintf(ku, sizeof ku, "sky_p%du", i);
        snprintf(kr, sizeof kr, "sky_p%dr", i);
        snprintf(kg, sizeof kg, "sky_p%dg", i);
        snprintf(kb, sizeof kb, "sky_p%db", i);
        snprintf(kn, sizeof kn, "sky_p%dn", i);
        load_u8(h, ku, &g_sky_preset[i].used, 0, 1);
        load_u8(h, kr, &g_sky_preset[i].r, 0, 255);
        load_u8(h, kg, &g_sky_preset[i].g, 0, 255);
        load_u8(h, kb, &g_sky_preset[i].b, 0, 255);
        size_t n = sizeof g_sky_preset[i].name;
        if (nvs_get_str(h, kn, g_sky_preset[i].name, &n) != ESP_OK)
            g_sky_preset[i].name[0] = 0;
    }
    if (legacy && g_sky_awb == SKY_AWB_PRESET0) {
        g_sky_preset[0].used = 1;
        g_sky_preset[0].r = g_sky_awb_r;
        g_sky_preset[0].g = g_sky_awb_g;
        g_sky_preset[0].b = g_sky_awb_b;
        snprintf(g_sky_preset[0].name, sizeof g_sky_preset[0].name, "Custom");
    }
    int slot = preset_slot(g_sky_awb);
    if (slot >= 0 && !g_sky_preset[slot].used)
        g_sky_awb = 0;
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
    nvs_set_u8(h, "sky_awb_r", g_sky_awb_r);
    nvs_set_u8(h, "sky_awb_g", g_sky_awb_g);
    nvs_set_u8(h, "sky_awb_b", g_sky_awb_b);
    nvs_set_u16(h, "sky_daym", g_sky_daym);
    for (int i = 0; i < SKY_AWB_PRESET_N; i++) {
        char ku[8], kr[8], kg[8], kb[8], kn[8];
        snprintf(ku, sizeof ku, "sky_p%du", i);
        snprintf(kr, sizeof kr, "sky_p%dr", i);
        snprintf(kg, sizeof kg, "sky_p%dg", i);
        snprintf(kb, sizeof kb, "sky_p%db", i);
        snprintf(kn, sizeof kn, "sky_p%dn", i);
        nvs_set_u8(h, ku, g_sky_preset[i].used);
        nvs_set_u8(h, kr, g_sky_preset[i].r);
        nvs_set_u8(h, kg, g_sky_preset[i].g);
        nvs_set_u8(h, kb, g_sky_preset[i].b);
        nvs_set_str(h, kn, g_sky_preset[i].name);
    }
}
