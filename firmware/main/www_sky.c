#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdlib.h>
#include <string.h>

#include "hp10_bringup.h"
#include "sky_cfg.h"
#include "sky_stats.h"
#include "upload_priv.h"
#include "www_priv.h"

static const char *TAG = "sky";

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

static esp_err_t send_cached(httpd_req_t *req)
{
    char buf[HP10_SKY_JSON_MAX];
    if (!hp10_sky_latest(buf, sizeof buf)) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "valid", 0);
        return send_json(req, o);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, strlen(buf));
}

static bool fresh_busy(void)
{
    static int64_t s_us;
    int64_t now = esp_timer_get_time();
    if (s_us && now - s_us < 5000000)
        return true;
    s_us = now;
    return false;
}

static esp_err_t measure_fresh(httpd_req_t *req)
{
    if (!g_sky_en) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "status", 0);
        cJSON_AddStringToObject(o, "msg", "sky stats disabled");
        return send_json(req, o);
    }
    if (!g_bCameraOk)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no camera");
    if (fresh_busy()) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "status", 0);
        cJSON_AddStringToObject(o, "msg", "busy");
        return send_json(req, o);
    }

    camera_fb_t *fb = grab_frame();
    if (!fb)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no camera");
    char json[HP10_SKY_JSON_MAX];
    esp_err_t err = hp10_sky_compute(fb->buf, fb->len,
                                     (uint16_t)fb->width, (uint16_t)fb->height,
                                     json, sizeof json);
    drop_frame(fb);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fresh %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sky compute failed");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, strlen(json));
}

static esp_err_t get_sky_stats(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    char q[64];
    char v[8];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "fresh", v, sizeof v) == ESP_OK &&
        strcmp(v, "1") == 0)
        return measure_fresh(req);
    return send_cached(req);
}

static esp_err_t get_sky_cfg(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "sky_en", g_sky_en);
    cJSON_AddNumberToObject(o, "sky_x", g_sky_x);
    cJSON_AddNumberToObject(o, "sky_y", g_sky_y);
    cJSON_AddNumberToObject(o, "sky_w", g_sky_w);
    cJSON_AddNumberToObject(o, "sky_h", g_sky_h);
    cJSON_AddNumberToObject(o, "sky_rb", g_sky_rb);
    cJSON_AddNumberToObject(o, "sky_sat", g_sky_sat);
    cJSON_AddNumberToObject(o, "sky_awb", g_sky_awb);
    cJSON_AddNumberToObject(o, "sky_daym", g_sky_daym);
    return send_json(req, o);
}

static esp_err_t reject_cfg(httpd_req_t *req, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "status", 0);
    cJSON_AddStringToObject(o, "msg", msg);
    return send_json(req, o);
}

static esp_err_t set_sky_cfg(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    char *body = recv_body(req);
    cJSON *in = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!in)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);

    int en, x, y, w, h, rb, sat, awb, daym;
    bool ok = json_int(in, "sky_en", &en) &&
              json_int(in, "sky_x", &x) &&
              json_int(in, "sky_y", &y) &&
              json_int(in, "sky_w", &w) &&
              json_int(in, "sky_h", &h) &&
              json_int(in, "sky_rb", &rb) &&
              json_int(in, "sky_sat", &sat) &&
              json_int(in, "sky_awb", &awb) &&
              json_int(in, "sky_daym", &daym);
    cJSON_Delete(in);
    if (!ok)
        return reject_cfg(req, "missing sky setting");
    const char *why = hp10_sky_cfg_apply(en, x, y, w, h, rb, sat, awb, daym);
    if (why)
        return reject_cfg(req, why);
    hp10_cfg_save();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "status", 1);
    cJSON_AddStringToObject(o, "msg", "ok");
    return send_json(req, o);
}

void www_sky_register(httpd_handle_t http)
{
    register_get(http, "/get_sky_stats", get_sky_stats);
    register_get(http, "/get_sky_cfg", get_sky_cfg);
    register_post(http, "/set_sky_cfg", set_sky_cfg);
}
