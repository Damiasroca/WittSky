#include "cJSON.h"
#include "esp_http_server.h"

#include <stdlib.h>
#include <string.h>

#include "hp10_bringup.h"
#include "overlay_cfg.h"
#include "www_priv.h"

static const char *el_name[OV_EL_N] = { "ts", "wd", "tp", "rn", "rs", "nd" };

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

static esp_err_t reject(httpd_req_t *req, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "status", 0);
    cJSON_AddStringToObject(o, "msg", msg);
    return send_json(req, o);
}

static esp_err_t get_overlay_cfg(httpd_req_t *req)
{
    cJSON *els;
    if (guest_json(req))
        return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "ov_en", g_ov_en);
    cJSON_AddNumberToObject(o, "ov_sta", g_ov_sta);
    cJSON_AddStringToObject(o, "ov_host", g_ov_host);
    cJSON_AddStringToObject(o, "ov_url", g_ov_url);
    cJSON_AddNumberToObject(o, "ov_port", g_ov_port);
    cJSON_AddNumberToObject(o, "ov_timeout", g_ov_timeout);
    cJSON_AddNumberToObject(o, "ov_wunit", g_ov_geom.wind_unit);
    cJSON_AddNumberToObject(o, "ov_tunit", g_ov_geom.temp_unit);
    cJSON_AddNumberToObject(o, "ov_clean", g_ov_clean);
    els = cJSON_AddArrayToObject(o, "el");
    for (int i = 0; i < OV_EL_N; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", el_name[i]);
        cJSON_AddNumberToObject(e, "en", g_ov_geom.en[i]);
        cJSON_AddNumberToObject(e, "an", g_ov_geom.an[i]);
        cJSON_AddNumberToObject(e, "x", g_ov_geom.ox[i]);
        cJSON_AddNumberToObject(e, "y", g_ov_geom.oy[i]);
        cJSON_AddItemToArray(els, e);
    }
    return send_json(req, o);
}

static esp_err_t set_overlay_cfg(httpd_req_t *req)
{
    char *body;
    cJSON *in;
    cJSON *els;
    int en, sta, clean, port, timeout, wunit, tunit;
    uint8_t en_el[OV_EL_N], an_el[OV_EL_N];
    int ox[OV_EL_N], oy[OV_EL_N];
    char host[64];
    char url[sizeof g_ov_url];
    const char *why;
    if (guest_json(req))
        return ESP_OK;
    body = recv_body(req);
    in = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!in)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    host[0] = 0;
    url[0] = 0;
    {
        cJSON *h = cJSON_GetObjectItem(in, "ov_host");
        if (cJSON_IsString(h) && h->valuestring)
            strlcpy(host, h->valuestring, sizeof host);
        h = cJSON_GetObjectItem(in, "ov_url");
        if (cJSON_IsString(h) && h->valuestring)
            strlcpy(url, h->valuestring, sizeof url);
    }
    if (!json_int(in, "ov_en", &en) || !json_int(in, "ov_sta", &sta) ||
        !json_int(in, "ov_clean", &clean) || !json_int(in, "ov_port", &port) ||
        !json_int(in, "ov_timeout", &timeout) || !json_int(in, "ov_wunit", &wunit) ||
        !json_int(in, "ov_tunit", &tunit)) {
        cJSON_Delete(in);
        return reject(req, "missing overlay setting");
    }
    els = cJSON_GetObjectItem(in, "el");
    if (!cJSON_IsArray(els) || cJSON_GetArraySize(els) != OV_EL_N) {
        cJSON_Delete(in);
        return reject(req, "missing overlay elements");
    }
    for (int i = 0; i < OV_EL_N; i++) {
        cJSON *e = cJSON_GetArrayItem(els, i);
        int een, ean, ex, ey;
        cJSON *id = cJSON_GetObjectItem(e, "id");
        if (!id || !cJSON_IsString(id) || !id->valuestring ||
            strcmp(id->valuestring, el_name[i]) != 0 ||
            !json_int(e, "en", &een) || !json_int(e, "an", &ean) ||
            !json_int(e, "x", &ex) || !json_int(e, "y", &ey)) {
            cJSON_Delete(in);
            return reject(req, "missing overlay element");
        }
        en_el[i] = (uint8_t)een;
        an_el[i] = (uint8_t)ean;
        ox[i] = ex;
        oy[i] = ey;
    }
    cJSON_Delete(in);
    why = hp10_overlay_cfg_apply(en, sta, clean, port, timeout, wunit, tunit, host, url,
                                 en_el, an_el, ox, oy);
    if (why)
        return reject(req, why);
    hp10_cfg_save();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "status", 1);
    cJSON_AddStringToObject(o, "msg", "ok");
    return send_json(req, o);
}

void www_overlay_register(httpd_handle_t http)
{
    register_get(http, "/get_overlay_cfg", get_overlay_cfg);
    register_post(http, "/set_overlay_cfg", set_overlay_cfg);
}
