#include "cJSON.h"
#include "esp_http_server.h"

#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"

#include "hp10_bringup.h"
#include "overlay.h"
#include "overlay_cfg.h"
#include "www_priv.h"

static const char *fixed_name[] = { "ts", "rs", "nd", "rn" };
static const int fixed_el[] = { OV_EL_TS, OV_EL_ROSE, OV_EL_NEEDLE, OV_EL_RAIN };

static bool json_int(cJSON *in, const char *key, int *out)
{
    cJSON *it = cJSON_GetObjectItem(in, key);
    if (!it)
        return false;
    if (cJSON_IsString(it)) {
        if (!it->valuestring)
            return false;
        *out = atoi(it->valuestring);
        return true;
    }
    if (!cJSON_IsNumber(it))
        return false;
    *out = it->valueint;
    return true;
}

static void json_str(cJSON *in, const char *key, char *dst, size_t n)
{
    cJSON *it = cJSON_GetObjectItem(in, key);
    dst[0] = 0;
    if (cJSON_IsString(it) && it->valuestring)
        strlcpy(dst, it->valuestring, n);
}

static esp_err_t reject(httpd_req_t *req, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "status", 0);
    cJSON_AddStringToObject(o, "msg", msg);
    return send_json(req, o);
}

static int fixed_index(const char *id)
{
    for (int i = 0; i < 4; i++) {
        if (strcmp(id, fixed_name[i]) == 0)
            return fixed_el[i];
    }
    return -1;
}

static esp_err_t get_overlay_cfg(httpd_req_t *req)
{
    cJSON *slots;
    cJSON *fixed;
    if (guest_json(req))
        return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "ov_en", g_ov_en);
    cJSON_AddNumberToObject(o, "ov_prov", g_ov_prov);
    cJSON_AddNumberToObject(o, "ov_sub", g_ov_sub);
    cJSON_AddStringToObject(o, "ov_host", g_wx.host);
    cJSON_AddNumberToObject(o, "ov_port", g_wx.port);
    cJSON_AddStringToObject(o, "ov_app_key", g_wx.app_key);
    cJSON_AddStringToObject(o, "ov_api_key", g_wx.api_key);
    cJSON_AddStringToObject(o, "ov_api_secret", g_wx.api_secret);
    cJSON_AddStringToObject(o, "ov_token", g_wx.token);
    cJSON_AddStringToObject(o, "ov_station", g_wx.station_id);
    cJSON_AddStringToObject(o, "ov_url", g_wx.url);
    cJSON_AddNumberToObject(o, "ov_timeout", g_ov_timeout);
    cJSON_AddNumberToObject(o, "ov_cache_ms", g_ov_cache_ms);
    cJSON_AddNumberToObject(o, "ov_wunit", g_ov_geom.wind_unit);
    cJSON_AddNumberToObject(o, "ov_tunit", g_ov_geom.temp_unit);
    cJSON_AddNumberToObject(o, "ov_punit", g_ov_geom.press_unit);
    cJSON_AddNumberToObject(o, "ov_runit", g_ov_geom.rain_unit);
    cJSON_AddNumberToObject(o, "ov_clean", g_ov_clean);
    slots = cJSON_AddArrayToObject(o, "slots");
    for (int i = 0; i < OV_SLOT_N; i++) {
        int el = OV_EL_M0 + i;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "id", i);
        cJSON_AddNumberToObject(e, "en", g_ov_geom.en[el]);
        cJSON_AddNumberToObject(e, "an", g_ov_geom.an[el]);
        cJSON_AddNumberToObject(e, "x", g_ov_geom.ox[el]);
        cJSON_AddNumberToObject(e, "y", g_ov_geom.oy[el]);
        cJSON_AddNumberToObject(e, "meas", g_ov_geom.slot_meas[i]);
        cJSON_AddItemToArray(slots, e);
    }
    fixed = cJSON_AddArrayToObject(o, "tsrose");
    for (int i = 0; i < 4; i++) {
        int el = fixed_el[i];
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", fixed_name[i]);
        cJSON_AddNumberToObject(e, "en", g_ov_geom.en[el]);
        cJSON_AddNumberToObject(e, "an", g_ov_geom.an[el]);
        cJSON_AddNumberToObject(e, "x", g_ov_geom.ox[el]);
        cJSON_AddNumberToObject(e, "y", g_ov_geom.oy[el]);
        cJSON_AddItemToArray(fixed, e);
    }
    return send_json(req, o);
}

static esp_err_t set_overlay_cfg(httpd_req_t *req)
{
    char *body;
    cJSON *in;
    cJSON *slots;
    cJSON *fixed;
    ov_cfg_in_t cfg;
    char host[64];
    char app_key[64];
    char api_key[64];
    char api_secret[64];
    char token[128];
    char station[48];
    int got_el[OV_EL_N];
    int got_slot[OV_SLOT_N];
    const char *why;
    if (guest_json(req))
        return ESP_OK;
    body = recv_body(req);
    in = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!in)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    memset(&cfg, 0, sizeof cfg);
    memset(got_el, 0, sizeof got_el);
    memset(got_slot, 0, sizeof got_slot);
    json_str(in, "ov_host", host, sizeof host);
    json_str(in, "ov_app_key", app_key, sizeof app_key);
    json_str(in, "ov_api_key", api_key, sizeof api_key);
    json_str(in, "ov_api_secret", api_secret, sizeof api_secret);
    json_str(in, "ov_token", token, sizeof token);
    json_str(in, "ov_station", station, sizeof station);
    cfg.host = host;
    cfg.app_key = app_key;
    cfg.api_key = api_key;
    cfg.api_secret = api_secret;
    cfg.token = token;
    cfg.station_id = station;
    if (!json_int(in, "ov_en", &cfg.en) || !json_int(in, "ov_prov", &cfg.prov) ||
        !json_int(in, "ov_sub", &cfg.sub) || !json_int(in, "ov_clean", &cfg.clean) ||
        !json_int(in, "ov_port", &cfg.port) || !json_int(in, "ov_timeout", &cfg.timeout) ||
        !json_int(in, "ov_cache_ms", &cfg.cache_ms) || !json_int(in, "ov_wunit", &cfg.wind_unit) ||
        !json_int(in, "ov_tunit", &cfg.temp_unit) || !json_int(in, "ov_punit", &cfg.press_unit) ||
        !json_int(in, "ov_runit", &cfg.rain_unit)) {
        cJSON_Delete(in);
        return reject(req, "missing overlay setting");
    }
    slots = cJSON_GetObjectItem(in, "slots");
    fixed = cJSON_GetObjectItem(in, "tsrose");
    if (!cJSON_IsArray(slots) || cJSON_GetArraySize(slots) != OV_SLOT_N ||
        !cJSON_IsArray(fixed) || cJSON_GetArraySize(fixed) != 4) {
        cJSON_Delete(in);
        return reject(req, "missing overlay elements");
    }
    for (int i = 0; i < OV_SLOT_N; i++) {
        cJSON *e = cJSON_GetArrayItem(slots, i);
        int id, een, ean, ex, ey, meas;
        if (!json_int(e, "id", &id) || id < 0 || id >= OV_SLOT_N || got_slot[id] ||
            !json_int(e, "en", &een) || !json_int(e, "an", &ean) ||
            !json_int(e, "x", &ex) || !json_int(e, "y", &ey) ||
            !json_int(e, "meas", &meas)) {
            cJSON_Delete(in);
            return reject(req, "missing overlay element");
        }
        got_slot[id] = 1;
        cfg.en_el[OV_EL_M0 + id] = (uint8_t)een;
        cfg.an_el[OV_EL_M0 + id] = (uint8_t)ean;
        cfg.ox[OV_EL_M0 + id] = ex;
        cfg.oy[OV_EL_M0 + id] = ey;
        cfg.slot_meas[id] = (uint8_t)meas;
    }
    for (int i = 0; i < 4; i++) {
        cJSON *e = cJSON_GetArrayItem(fixed, i);
        cJSON *idj = cJSON_GetObjectItem(e, "id");
        int el, een, ean, ex, ey;
        if (!idj || !cJSON_IsString(idj) || !idj->valuestring)
            el = -1;
        else
            el = fixed_index(idj->valuestring);
        if (el < 0 || got_el[el] || !json_int(e, "en", &een) || !json_int(e, "an", &ean) ||
            !json_int(e, "x", &ex) || !json_int(e, "y", &ey)) {
            cJSON_Delete(in);
            return reject(req, "missing overlay element");
        }
        got_el[el] = 1;
        cfg.en_el[el] = (uint8_t)een;
        cfg.an_el[el] = (uint8_t)ean;
        cfg.ox[el] = ex;
        cfg.oy[el] = ey;
    }
    cJSON_Delete(in);
    why = hp10_overlay_cfg_apply(&cfg);
    if (why)
        return reject(req, why);
    hp10_cfg_save();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "status", 1);
    cJSON_AddStringToObject(o, "msg", "ok");
    return send_json(req, o);
}

static bool wx_probe_busy(void)
{
    static int64_t s_us;
    int64_t now = esp_timer_get_time();
    if (s_us && now - s_us < 5000000)
        return true;
    s_us = now;
    return false;
}

static esp_err_t get_overlay_wx(httpd_req_t *req)
{
    char q[64];
    char v[8];
    const char *why = NULL;
    int fresh = 0;
    cJSON *o;

    if (guest_json(req))
        return ESP_OK;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "fresh", v, sizeof v) == ESP_OK &&
        strcmp(v, "1") == 0)
        fresh = 1;
    if (fresh) {
        if (wx_probe_busy()) {
            httpd_resp_set_status(req, "429 Too Many Requests");
            o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "status", 0);
            cJSON_AddStringToObject(o, "msg", "busy");
            hp10_overlay_wx_add_json(o);
            return send_json(req, o);
        }
        if (hp10_overlay_wx_probe(&why) != ESP_OK) {
            o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "status", 0);
            cJSON_AddStringToObject(o, "msg", why ? why : "station fetch failed");
            hp10_overlay_wx_add_json(o);
            return send_json(req, o);
        }
    }
    o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "status", 1);
    cJSON_AddStringToObject(o, "msg", "ok");
    hp10_overlay_wx_add_json(o);
    return send_json(req, o);
}

void www_overlay_register(httpd_handle_t http)
{
    register_get(http, "/get_overlay_cfg", get_overlay_cfg);
    register_post(http, "/set_overlay_cfg", set_overlay_cfg);
    register_get(http, "/get_overlay_wx", get_overlay_wx);
}
