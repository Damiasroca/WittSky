#include "overlay_cfg.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "overlay";

uint8_t  g_ov_en;
uint8_t  g_ov_sta = 2;
uint8_t  g_ov_clean;
uint16_t g_ov_port = 80;
uint16_t g_ov_timeout = 8000;
char     g_ov_host[64];
char     g_ov_url[384] =
    "https://api.ecowitt.net/api/v3/device/real_time"
    "?application_key=D1F49A4662E4E887AE1632C7ABA55A7D"
    "&api_key=cd12cabb-ec99-4709-8789-5be424a64d47"
    "&mac=30:C9:22:3D:84:9B"
    "&call_back=all&temp_unitid=1&pressure_unitid=3"
    "&wind_speed_unitid=7&rainfall_unitid=12"
    "&solar_irradiance_unitid=16&capacity_unitid=24";
ov_geom_t g_ov_geom;

static const char *el_tag[OV_EL_N] = { "ts", "wd", "tp", "rn", "rs", "nd" };

static int in_range(int v, int lo, int hi)
{
    return v >= lo && v <= hi;
}

static int host_ok(const char *s)
{
    if (!s || !s[0] || strlen(s) >= sizeof g_ov_host)
        return 0;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-')
            continue;
        return 0;
    }
    return 1;
}

static int url_ok(const char *s)
{
    size_t n;
    if (!s || strncmp(s, "https://", 8) != 0)
        return 0;
    n = strlen(s);
    if (n < 16 || n >= sizeof g_ov_url)
        return 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9'))
            continue;
        if (c == ':' || c == '/' || c == '?' || c == '&' || c == '=' ||
            c == '%' || c == '_' || c == '-' || c == '.' || c == '~' || c == '+')
            continue;
        return 0;
    }
    return 1;
}

const char *hp10_overlay_cfg_apply(int en, int sta, int clean, int port, int timeout,
                                   int wind_unit, int temp_unit, const char *host,
                                   const char *url,
                                   const uint8_t en_el[OV_EL_N],
                                   const uint8_t an_el[OV_EL_N],
                                   const int ox[OV_EL_N], const int oy[OV_EL_N])
{
    if (!in_range(en, 0, 1) || !in_range(sta, 0, 2) || !in_range(clean, 0, 1))
        return "overlay switch must be 0 or 1";
    if (!in_range(port, 1, 65535))
        return "port must be 1-65535";
    if (!in_range(timeout, 500, 20000))
        return "timeout must be 500-20000 ms";
    if (!in_range(wind_unit, 0, 3))
        return "wind unit must be 0-3";
    if (!in_range(temp_unit, 0, 1))
        return "temperature unit must be 0 or 1";
    if (sta == 1 && !host_ok(host))
        return "station host must be a hostname or IPv4 address";
    if (host && host[0] && !host_ok(host))
        return "station host must be a hostname or IPv4 address";
    if (sta == 2 && !url_ok(url))
        return "cloud URL must be https";
    if (url && url[0] && !url_ok(url))
        return "cloud URL must be https";
    for (int i = 0; i < OV_EL_N; i++) {
        if (!in_range(en_el[i], 0, 1))
            return "element enable must be 0 or 1";
        if (!in_range(an_el[i], 0, OV_AN_N - 1))
            return "anchor must be 0-7";
        if (!in_range(ox[i], -1000, 1000) || !in_range(oy[i], -1000, 1000))
            return "offset must be -1000 to 1000";
    }

    g_ov_en = (uint8_t)en;
    g_ov_sta = (uint8_t)sta;
    g_ov_clean = (uint8_t)clean;
    g_ov_port = (uint16_t)port;
    g_ov_timeout = (uint16_t)timeout;
    g_ov_host[0] = 0;
    if (host && host[0])
        strlcpy(g_ov_host, host, sizeof g_ov_host);
    if (url && url[0])
        strlcpy(g_ov_url, url, sizeof g_ov_url);
    for (int i = 0; i < OV_EL_N; i++) {
        g_ov_geom.en[i] = en_el[i];
        g_ov_geom.an[i] = an_el[i];
        g_ov_geom.ox[i] = (int16_t)ox[i];
        g_ov_geom.oy[i] = (int16_t)oy[i];
    }
    g_ov_geom.wind_unit = (uint8_t)wind_unit;
    g_ov_geom.temp_unit = (uint8_t)temp_unit;
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

static void load_i16(nvs_handle_t h, const char *key, int16_t *dst, int lo, int hi)
{
    int16_t v;
    if (nvs_get_i16(h, key, &v) != ESP_OK)
        return;
    if (in_range(v, lo, hi))
        *dst = v;
    else
        ESP_LOGW(TAG, "nvs %s=%d out of range", key, (int)v);
}

void hp10_overlay_cfg_load(nvs_handle_t h)
{
    ov_geom_defaults(&g_ov_geom);
    load_u8(h, "ov_en", &g_ov_en, 0, 1);
    load_u8(h, "ov_sta", &g_ov_sta, 0, 2);
    load_u8(h, "ov_cln", &g_ov_clean, 0, 1);
    load_u16(h, "ov_port", &g_ov_port, 1, 65535);
    load_u16(h, "ov_to", &g_ov_timeout, 500, 20000);
    load_u8(h, "ov_wu", &g_ov_geom.wind_unit, 0, 3);
    load_u8(h, "ov_tu", &g_ov_geom.temp_unit, 0, 1);
    size_t n = sizeof g_ov_host;
    if (nvs_get_str(h, "ov_host", g_ov_host, &n) != ESP_OK || !host_ok(g_ov_host))
        g_ov_host[0] = 0;
    {
        char saved[sizeof g_ov_url];
        size_t un = sizeof saved;
        if (nvs_get_str(h, "ov_url", saved, &un) == ESP_OK && url_ok(saved))
            strlcpy(g_ov_url, saved, sizeof g_ov_url);
    }
    for (int i = 0; i < OV_EL_N; i++) {
        char k[12];
        snprintf(k, sizeof k, "ov_%s_e", el_tag[i]);
        load_u8(h, k, &g_ov_geom.en[i], 0, 1);
        snprintf(k, sizeof k, "ov_%s_a", el_tag[i]);
        load_u8(h, k, &g_ov_geom.an[i], 0, OV_AN_N - 1);
        snprintf(k, sizeof k, "ov_%s_x", el_tag[i]);
        load_i16(h, k, &g_ov_geom.ox[i], -1000, 1000);
        snprintf(k, sizeof k, "ov_%s_y", el_tag[i]);
        load_i16(h, k, &g_ov_geom.oy[i], -1000, 1000);
    }
    if (g_ov_sta == 1 && !g_ov_host[0])
        g_ov_sta = 0;
    if (g_ov_sta == 2 && !url_ok(g_ov_url))
        g_ov_sta = 0;
}

void hp10_overlay_cfg_save(nvs_handle_t h)
{
    nvs_set_u8(h, "ov_en", g_ov_en);
    nvs_set_u8(h, "ov_sta", g_ov_sta);
    nvs_set_u8(h, "ov_cln", g_ov_clean);
    nvs_set_u16(h, "ov_port", g_ov_port);
    nvs_set_u16(h, "ov_to", g_ov_timeout);
    nvs_set_u8(h, "ov_wu", g_ov_geom.wind_unit);
    nvs_set_u8(h, "ov_tu", g_ov_geom.temp_unit);
    nvs_set_str(h, "ov_host", g_ov_host);
    nvs_set_str(h, "ov_url", g_ov_url);
    for (int i = 0; i < OV_EL_N; i++) {
        char k[12];
        snprintf(k, sizeof k, "ov_%s_e", el_tag[i]);
        nvs_set_u8(h, k, g_ov_geom.en[i]);
        snprintf(k, sizeof k, "ov_%s_a", el_tag[i]);
        nvs_set_u8(h, k, g_ov_geom.an[i]);
        snprintf(k, sizeof k, "ov_%s_x", el_tag[i]);
        nvs_set_i16(h, k, g_ov_geom.ox[i]);
        snprintf(k, sizeof k, "ov_%s_y", el_tag[i]);
        nvs_set_i16(h, k, g_ov_geom.oy[i]);
    }
}
