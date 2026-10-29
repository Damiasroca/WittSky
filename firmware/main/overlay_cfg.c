#include "overlay_cfg.h"

#include "esp_log.h"
#include "overlay.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "overlay";

static StaticSemaphore_t s_mu_buf;
static SemaphoreHandle_t s_mu;

static void mu_ensure(void)
{
    if (!s_mu)
        s_mu = xSemaphoreCreateMutexStatic(&s_mu_buf);
}

static void mu_take(void)
{
    mu_ensure();
    xSemaphoreTake(s_mu, portMAX_DELAY);
}

static void mu_give(void)
{
    xSemaphoreGive(s_mu);
}

uint8_t  g_ov_en;
uint8_t  g_ov_prov = OV_PROV_ECO;
uint8_t  g_ov_sub = OV_SUB_CLOUD;
uint8_t  g_ov_clean;
uint32_t g_ov_cache_ms = 30000;
uint16_t g_ov_timeout = 8000;
wx_cfg_t g_wx = {
    .port = 80,
    .url =
        "https://api.ecowitt.net/api/v3/device/real_time"
        "?application_key=D1F49A4662E4E887AE1632C7ABA55A7D"
        "&api_key=cd12cabb-ec99-4709-8789-5be424a64d47"
        "&mac=30:C9:22:3D:84:9B"
        "&call_back=all&temp_unitid=1&pressure_unitid=3"
        "&wind_speed_unitid=6&rainfall_unitid=12"
        "&solar_irradiance_unitid=16",
};
ov_geom_t g_ov_geom;

static const char *el_tag[OV_EL_N] = {
    "ts", "m0", "m1", "m2", "m3", "m4", "rs", "nd", "rn"
};

static int in_range(int v, int lo, int hi)
{
    return v >= lo && v <= hi;
}

static int host_ok(const char *s)
{
    if (!s || !s[0] || strlen(s) >= sizeof g_wx.host)
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
    if (n < 16 || n >= sizeof g_wx.url)
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

static int cred_ok(const char *s, size_t cap, int colon)
{
    size_t n;
    if (!s || !s[0])
        return 1;
    n = strlen(s);
    if (n >= cap)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
            c == '~' || c == '+' || c == '=')
            continue;
        if (colon && c == ':')
            continue;
        return 0;
    }
    return 1;
}

static void copy_cred(char *dst, size_t n, const char *src)
{
    dst[0] = 0;
    if (src && src[0])
        strlcpy(dst, src, n);
}

const char *hp10_overlay_cfg_apply(const ov_cfg_in_t *in)
{
    int eco_keys;
    int keep_url;
    if (!in)
        return "missing overlay setting";
    if (!in_range(in->en, 0, 1) || !in_range(in->clean, 0, 1))
        return "overlay switch must be 0 or 1";
    if (!in_range(in->prov, 0, OV_PROV_N - 1) || !in_range(in->sub, 0, 1))
        return "station must be a known provider";
    if (!in_range(in->port, 1, 65535))
        return "port must be 1-65535";
    if (!in_range(in->timeout, 500, 20000))
        return "timeout must be 500-20000 ms";
    if (!in_range(in->cache_ms, 10000, 300000))
        return "cache must be 10-300 s";
    if (!in_range(in->wind_unit, 0, 3))
        return "wind unit must be 0-3";
    if (!in_range(in->temp_unit, 0, 1))
        return "temperature unit must be 0 or 1";
    if (!in_range(in->press_unit, 0, 2))
        return "pressure unit must be 0-2";
    if (!in_range(in->rain_unit, 0, 1))
        return "rain unit must be 0 or 1";
    if (in->host && in->host[0] && !host_ok(in->host))
        return "station host must be a hostname or IPv4 address";
    if (!cred_ok(in->app_key, sizeof g_wx.app_key, 0))
        return "application key";
    if (!cred_ok(in->api_key, sizeof g_wx.api_key, 0))
        return "API key";
    if (!cred_ok(in->api_secret, sizeof g_wx.api_secret, 0))
        return "API secret";
    if (!cred_ok(in->token, sizeof g_wx.token, 0))
        return "token";
    if (!cred_ok(in->station_id, sizeof g_wx.station_id, 1))
        return "station id";
    eco_keys = in->app_key && in->app_key[0] && in->api_key && in->api_key[0] &&
               in->station_id && in->station_id[0];
    if ((in->prov == OV_PROV_ECO || in->prov == OV_PROV_WL) && in->sub == OV_SUB_LOCAL &&
        !host_ok(in->host))
        return "station host must be a hostname or IPv4 address";
    if (in->prov == OV_PROV_ECO && in->sub == OV_SUB_CLOUD && !eco_keys && !url_ok(g_wx.url))
        return "Ecowitt cloud needs an application key, API key, and station MAC";
    if (in->prov == OV_PROV_WL && in->sub == OV_SUB_CLOUD &&
        !(in->api_key && in->api_key[0] && in->api_secret && in->api_secret[0] &&
          in->station_id && in->station_id[0]))
        return "WeatherLink cloud needs an API key, API secret, and station ID";
    if (in->prov == OV_PROV_AWN &&
        !(in->app_key && in->app_key[0] && in->api_key && in->api_key[0]))
        return "Ambient Weather needs an application key and an API key";
    if (in->prov == OV_PROV_TEMPEST &&
        !(in->token && in->token[0] && in->station_id && in->station_id[0]))
        return "Tempest needs a token and a station ID";
    if (in->prov == OV_PROV_WU &&
        !(in->api_key && in->api_key[0] && in->station_id && in->station_id[0]))
        return "Weather Underground needs a station ID and an API key";
    for (int i = 0; i < OV_EL_N; i++) {
        if (!in_range(in->en_el[i], 0, 1))
            return "element enable must be 0 or 1";
        if (!in_range(in->an_el[i], 0, OV_AN_N - 1))
            return "anchor must be 0-7";
        if (!in_range(in->ox[i], -1000, 1000) || !in_range(in->oy[i], -1000, 1000))
            return "offset must be -1000 to 1000";
    }
    for (int i = 0; i < OV_SLOT_N; i++) {
        if (!in_range(in->slot_meas[i], 0, OV_MEAS_N - 1))
            return "measurement must be a known reading";
    }

    mu_take();
    g_ov_en = (uint8_t)in->en;
    g_ov_prov = (uint8_t)in->prov;
    g_ov_sub = (uint8_t)in->sub;
    g_ov_clean = (uint8_t)in->clean;
    g_wx.port = (uint16_t)in->port;
    g_ov_timeout = (uint16_t)in->timeout;
    g_ov_cache_ms = (uint32_t)in->cache_ms;
    copy_cred(g_wx.host, sizeof g_wx.host, in->host);
    copy_cred(g_wx.app_key, sizeof g_wx.app_key, in->app_key);
    copy_cred(g_wx.api_key, sizeof g_wx.api_key, in->api_key);
    copy_cred(g_wx.api_secret, sizeof g_wx.api_secret, in->api_secret);
    copy_cred(g_wx.token, sizeof g_wx.token, in->token);
    copy_cred(g_wx.station_id, sizeof g_wx.station_id, in->station_id);
    keep_url = in->prov == OV_PROV_ECO && in->sub == OV_SUB_CLOUD && !eco_keys &&
               url_ok(g_wx.url);
    if (!keep_url)
        g_wx.url[0] = 0;
    for (int i = 0; i < OV_EL_N; i++) {
        g_ov_geom.en[i] = in->en_el[i];
        g_ov_geom.an[i] = in->an_el[i];
        g_ov_geom.ox[i] = (int16_t)in->ox[i];
        g_ov_geom.oy[i] = (int16_t)in->oy[i];
    }
    for (int i = 0; i < OV_SLOT_N; i++)
        g_ov_geom.slot_meas[i] = in->slot_meas[i];
    g_ov_geom.wind_unit = (uint8_t)in->wind_unit;
    g_ov_geom.temp_unit = (uint8_t)in->temp_unit;
    g_ov_geom.press_unit = (uint8_t)in->press_unit;
    g_ov_geom.rain_unit = (uint8_t)in->rain_unit;
    mu_give();
    hp10_overlay_cache_clear();
    return NULL;
}

void hp10_overlay_cfg_snapshot(wx_cfg_t *wx, uint8_t *prov, uint8_t *sub,
                               uint16_t *timeout, uint32_t *cache_ms)
{
    mu_take();
    if (wx)
        *wx = g_wx;
    if (prov)
        *prov = g_ov_prov;
    if (sub)
        *sub = g_ov_sub;
    if (timeout)
        *timeout = g_ov_timeout;
    if (cache_ms)
        *cache_ms = g_ov_cache_ms;
    mu_give();
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

static void load_u32(nvs_handle_t h, const char *key, uint32_t *dst, uint32_t lo, uint32_t hi)
{
    uint32_t v;
    if (nvs_get_u32(h, key, &v) != ESP_OK)
        return;
    if (v >= lo && v <= hi)
        *dst = v;
    else
        ESP_LOGW(TAG, "nvs %s=%u out of range", key, (unsigned)v);
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

static void load_el(nvs_handle_t h, const char *tag, int idx)
{
    char k[12];
    snprintf(k, sizeof k, "ov_%s_e", tag);
    load_u8(h, k, &g_ov_geom.en[idx], 0, 1);
    snprintf(k, sizeof k, "ov_%s_a", tag);
    load_u8(h, k, &g_ov_geom.an[idx], 0, OV_AN_N - 1);
    snprintf(k, sizeof k, "ov_%s_x", tag);
    load_i16(h, k, &g_ov_geom.ox[idx], -1000, 1000);
    snprintf(k, sizeof k, "ov_%s_y", tag);
    load_i16(h, k, &g_ov_geom.oy[idx], -1000, 1000);
}

static void load_cred(nvs_handle_t h, const char *key, char *dst, size_t cap, int colon)
{
    char tmp[128];
    size_t n = sizeof tmp;
    if (cap > sizeof tmp)
        return;
    if (nvs_get_str(h, key, tmp, &n) != ESP_OK)
        return;
    if (!cred_ok(tmp, cap, colon))
        return;
    strlcpy(dst, tmp, cap);
}

static void query_arg(const char *url, const char *key, char *dst, size_t n)
{
    const char *p;
    size_t kn, i;
    if (!dst || n == 0)
        return;
    dst[0] = 0;
    if (!url || !key)
        return;
    kn = strlen(key);
    for (p = url; *p; p++) {
        if ((p == url || p[-1] == '?' || p[-1] == '&') &&
            strncmp(p, key, kn) == 0 && p[kn] == '=') {
            p += kn + 1;
            for (i = 0; p[i] && p[i] != '&' && i + 1 < n; i++)
                dst[i] = p[i];
            dst[i] = 0;
            return;
        }
    }
}

static void fill_from_url(void)
{
    char tmp[68];
    if (!g_wx.app_key[0]) {
        query_arg(g_wx.url, "application_key", tmp, sizeof tmp);
        if (tmp[0] && cred_ok(tmp, sizeof g_wx.app_key, 0))
            strlcpy(g_wx.app_key, tmp, sizeof g_wx.app_key);
    }
    if (!g_wx.api_key[0]) {
        query_arg(g_wx.url, "api_key", tmp, sizeof tmp);
        if (tmp[0] && cred_ok(tmp, sizeof g_wx.api_key, 0))
            strlcpy(g_wx.api_key, tmp, sizeof g_wx.api_key);
    }
    if (!g_wx.station_id[0]) {
        query_arg(g_wx.url, "mac", tmp, sizeof tmp);
        if (tmp[0] && cred_ok(tmp, sizeof g_wx.station_id, 1))
            strlcpy(g_wx.station_id, tmp, sizeof g_wx.station_id);
    }
}

static int keys_of(const char *a, const char *b, const char *c)
{
    return a && a[0] && b && b[0] && (!c || c[0]);
}

void hp10_overlay_cfg_load(nvs_handle_t h)
{
    uint8_t modern = 0;
    uint8_t v = 0;
    ov_geom_defaults(&g_ov_geom);
    load_u8(h, "ov_en", &g_ov_en, 0, 1);
    load_u8(h, "ov_cln", &g_ov_clean, 0, 1);
    load_u16(h, "ov_port", &g_wx.port, 1, 65535);
    load_u16(h, "ov_to", &g_ov_timeout, 500, 20000);
    load_u32(h, "ov_cache", &g_ov_cache_ms, 10000, 300000);
    load_u8(h, "ov_wu", &g_ov_geom.wind_unit, 0, 3);
    load_u8(h, "ov_tu", &g_ov_geom.temp_unit, 0, 1);
    load_u8(h, "ov_pu", &g_ov_geom.press_unit, 0, 2);
    load_u8(h, "ov_ru", &g_ov_geom.rain_unit, 0, 1);
    if (nvs_get_u8(h, "ov_prov", &v) == ESP_OK && in_range(v, 0, OV_PROV_N - 1)) {
        g_ov_prov = v;
        load_u8(h, "ov_sub", &g_ov_sub, 0, 1);
    } else if (nvs_get_u8(h, "ov_sta", &v) == ESP_OK) {
        if (v == 1) {
            g_ov_prov = OV_PROV_ECO;
            g_ov_sub = OV_SUB_LOCAL;
        } else if (v == 2) {
            g_ov_prov = OV_PROV_ECO;
            g_ov_sub = OV_SUB_CLOUD;
        } else {
            g_ov_prov = OV_PROV_NONE;
        }
    }
    {
        char saved[sizeof g_wx.host];
        size_t n = sizeof saved;
        if (nvs_get_str(h, "ov_host", saved, &n) == ESP_OK && host_ok(saved))
            strlcpy(g_wx.host, saved, sizeof g_wx.host);
    }
    {
        char saved[sizeof g_wx.url];
        size_t n = sizeof saved;
        esp_err_t e = nvs_get_str(h, "ov_url", saved, &n);
        if (e == ESP_OK) {
            if (url_ok(saved))
                strlcpy(g_wx.url, saved, sizeof g_wx.url);
            else
                g_wx.url[0] = 0;
        }
    }
    load_cred(h, "ov_appk", g_wx.app_key, sizeof g_wx.app_key, 0);
    load_cred(h, "ov_apik", g_wx.api_key, sizeof g_wx.api_key, 0);
    load_cred(h, "ov_apis", g_wx.api_secret, sizeof g_wx.api_secret, 0);
    load_cred(h, "ov_tok", g_wx.token, sizeof g_wx.token, 0);
    load_cred(h, "ov_sid", g_wx.station_id, sizeof g_wx.station_id, 1);
    if (nvs_get_u8(h, "ov_m0_e", &modern) == ESP_OK)
        modern = 1;
    else
        modern = 0;
    if (modern) {
        for (int i = 0; i < OV_SLOT_N; i++) {
            char k[12];
            load_el(h, el_tag[OV_EL_M0 + i], OV_EL_M0 + i);
            snprintf(k, sizeof k, "ov_m%d_k", i);
            load_u8(h, k, &g_ov_geom.slot_meas[i], 0, OV_MEAS_N - 1);
        }
    } else {
        load_el(h, "wd", OV_EL_M0);
        load_el(h, "tp", OV_EL_M1);
    }
    load_el(h, "ts", OV_EL_TS);
    load_el(h, "rs", OV_EL_ROSE);
    load_el(h, "nd", OV_EL_NEEDLE);
    load_el(h, "rn", OV_EL_RAIN);
    fill_from_url();
    if (g_ov_prov == OV_PROV_ECO && g_ov_sub == OV_SUB_LOCAL && !g_wx.host[0])
        g_ov_prov = OV_PROV_NONE;
    if (g_ov_prov == OV_PROV_ECO && g_ov_sub == OV_SUB_CLOUD &&
        !keys_of(g_wx.app_key, g_wx.api_key, g_wx.station_id) && !url_ok(g_wx.url))
        g_ov_prov = OV_PROV_NONE;
    if (g_ov_prov == OV_PROV_WL && g_ov_sub == OV_SUB_LOCAL && !g_wx.host[0])
        g_ov_prov = OV_PROV_NONE;
    if (g_ov_prov == OV_PROV_WL && g_ov_sub == OV_SUB_CLOUD &&
        !keys_of(g_wx.api_key, g_wx.api_secret, g_wx.station_id))
        g_ov_prov = OV_PROV_NONE;
    if (g_ov_prov == OV_PROV_AWN && !keys_of(g_wx.app_key, g_wx.api_key, NULL))
        g_ov_prov = OV_PROV_NONE;
    if (g_ov_prov == OV_PROV_TEMPEST && !keys_of(g_wx.token, g_wx.station_id, NULL))
        g_ov_prov = OV_PROV_NONE;
    if (g_ov_prov == OV_PROV_WU && !keys_of(g_wx.api_key, g_wx.station_id, NULL))
        g_ov_prov = OV_PROV_NONE;
}

void hp10_overlay_cfg_save(nvs_handle_t h)
{
    nvs_set_u8(h, "ov_en", g_ov_en);
    nvs_set_u8(h, "ov_prov", g_ov_prov);
    nvs_set_u8(h, "ov_sub", g_ov_sub);
    nvs_set_u8(h, "ov_cln", g_ov_clean);
    nvs_set_u16(h, "ov_port", g_wx.port);
    nvs_set_u16(h, "ov_to", g_ov_timeout);
    nvs_set_u32(h, "ov_cache", g_ov_cache_ms);
    nvs_set_u8(h, "ov_wu", g_ov_geom.wind_unit);
    nvs_set_u8(h, "ov_tu", g_ov_geom.temp_unit);
    nvs_set_u8(h, "ov_pu", g_ov_geom.press_unit);
    nvs_set_u8(h, "ov_ru", g_ov_geom.rain_unit);
    nvs_set_str(h, "ov_host", g_wx.host);
    nvs_set_str(h, "ov_url", g_wx.url);
    nvs_set_str(h, "ov_appk", g_wx.app_key);
    nvs_set_str(h, "ov_apik", g_wx.api_key);
    nvs_set_str(h, "ov_apis", g_wx.api_secret);
    nvs_set_str(h, "ov_tok", g_wx.token);
    nvs_set_str(h, "ov_sid", g_wx.station_id);
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
    for (int i = 0; i < OV_SLOT_N; i++) {
        char k[12];
        snprintf(k, sizeof k, "ov_m%d_k", i);
        nvs_set_u8(h, k, g_ov_geom.slot_meas[i]);
    }
    /* One-shot migration: wipe the pre-multistation keys the first time we
     * save under the flat schema, then set a sentinel so later saves skip the
     * extra NVS traffic.
     */
    {
        uint8_t mig = 0;
        if (nvs_get_u8(h, "ov_mig", &mig) != ESP_OK || mig != 1) {
            static const char *legacy[] = {
                "ov_sta",
                "ov_wd_e", "ov_wd_a", "ov_wd_x", "ov_wd_y",
                "ov_tp_e", "ov_tp_a", "ov_tp_x", "ov_tp_y",
            };
            for (size_t i = 0; i < sizeof legacy / sizeof legacy[0]; i++)
                nvs_erase_key(h, legacy[i]);
            nvs_set_u8(h, "ov_mig", 1);
        }
    }
}
