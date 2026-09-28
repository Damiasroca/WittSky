#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdlib.h>
#include <string.h>

#include "hp10_bringup.h"
#include "sky_awb.h"
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

static void add_presets(cJSON *o);

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
    cJSON_AddNumberToObject(o, "sky_awb_r", g_sky_awb_r);
    cJSON_AddNumberToObject(o, "sky_awb_g", g_sky_awb_g);
    cJSON_AddNumberToObject(o, "sky_awb_b", g_sky_awb_b);
    cJSON_AddNumberToObject(o, "sky_daym", g_sky_daym);
    add_presets(o);
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
    if (hp10_sky_awb_cal_busy())
        return reject_cfg(req, "Calibration is running.");
    char *body = recv_body(req);
    cJSON *in = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!in)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);

    int en, x, y, w, h, rb, sat, awb, daym;
    int awb_r = g_sky_awb_r, awb_g = g_sky_awb_g, awb_b = g_sky_awb_b;
    bool ok = json_int(in, "sky_en", &en) &&
              json_int(in, "sky_x", &x) &&
              json_int(in, "sky_y", &y) &&
              json_int(in, "sky_w", &w) &&
              json_int(in, "sky_h", &h) &&
              json_int(in, "sky_rb", &rb) &&
              json_int(in, "sky_sat", &sat) &&
              json_int(in, "sky_awb", &awb) &&
              json_int(in, "sky_daym", &daym);
    json_int(in, "sky_awb_r", &awb_r);
    json_int(in, "sky_awb_g", &awb_g);
    json_int(in, "sky_awb_b", &awb_b);
    cJSON_Delete(in);
    if (!ok)
        return reject_cfg(req, "missing sky setting");
    const char *why = hp10_sky_cfg_apply(en, x, y, w, h, rb, sat, awb,
                                         awb_r, awb_g, awb_b, daym);
    if (why)
        return reject_cfg(req, why);
    hp10_cfg_save();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "status", 1);
    cJSON_AddStringToObject(o, "msg", "ok");
    return send_json(req, o);
}

static void add_gains(cJSON *o, int r, int g, int b)
{
    cJSON *arr = cJSON_AddArrayToObject(o, "awb_gains");
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(r));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(g));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(b));
}

static void add_presets(cJSON *o)
{
    cJSON *arr = cJSON_AddArrayToObject(o, "presets");
    for (int i = 0; i < SKY_AWB_PRESET_N; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "used", g_sky_preset[i].used);
        cJSON_AddStringToObject(p, "name", g_sky_preset[i].name);
        cJSON_AddNumberToObject(p, "r", g_sky_preset[i].r);
        cJSON_AddNumberToObject(p, "g", g_sky_preset[i].g);
        cJSON_AddNumberToObject(p, "b", g_sky_preset[i].b);
        cJSON_AddItemToArray(arr, p);
    }
}

static esp_err_t get_awb(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "sky_awb", g_sky_awb);
    cJSON_AddNumberToObject(o, "sky_awb_r", g_sky_awb_r);
    cJSON_AddNumberToObject(o, "sky_awb_g", g_sky_awb_g);
    cJSON_AddNumberToObject(o, "sky_awb_b", g_sky_awb_b);
    add_presets(o);
    char cal_state[8];
    char cal_msg[80];
    hp10_sky_awb_cal_status(cal_state, sizeof cal_state, cal_msg, sizeof cal_msg);
    cJSON_AddStringToObject(o, "cal_state", cal_state);
    cJSON_AddStringToObject(o, "cal_msg", cal_msg);
    int r, g, b;
    if (!hp10_sky_awb_applied(&r, &g, &b))
        cJSON_AddNullToObject(o, "awb_gains");
    else
        add_gains(o, r, g, b);
    return send_json(req, o);
}

static esp_err_t set_awb(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    if (hp10_sky_awb_cal_busy())
        return reject_cfg(req, "Calibration is running.");
    char *body = recv_body(req);
    cJSON *in = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!in)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);

    int awb, r, g, b;
    bool ok = json_int(in, "sky_awb", &awb) &&
              json_int(in, "sky_awb_r", &r) &&
              json_int(in, "sky_awb_g", &g) &&
              json_int(in, "sky_awb_b", &b);
    cJSON_Delete(in);
    if (!ok)
        return reject_cfg(req, "missing awb setting");
    const char *why = hp10_sky_awb_set(awb, r, g, b);
    if (why)
        return reject_cfg(req, why);
    hp10_cfg_save();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "status", 1);
    cJSON_AddStringToObject(o, "msg", "ok");
    return send_json(req, o);
}

static const char *json_name(cJSON *in, const char **out)
{
    cJSON *it = cJSON_GetObjectItem(in, "name");
    if (!it || !cJSON_IsString(it) || !it->valuestring)
        return "Name the preset";
    *out = it->valuestring;
    return NULL;
}

static esp_err_t save_awb_preset(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    if (hp10_sky_awb_cal_busy())
        return reject_cfg(req, "Calibration is running.");
    char *body = recv_body(req);
    cJSON *in = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!in)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);

    const char *name = NULL;
    int r, g, b;
    const char *bad = json_name(in, &name);
    bool ok = !bad &&
              json_int(in, "r", &r) &&
              json_int(in, "g", &g) &&
              json_int(in, "b", &b);
    if (!ok) {
        cJSON_Delete(in);
        return reject_cfg(req, bad ? bad : "missing preset");
    }
    /* name points into the JSON tree; copy it before that tree is freed. */
    const char *why = hp10_sky_awb_preset_save(name, r, g, b);
    cJSON_Delete(in);
    if (why)
        return reject_cfg(req, why);
    hp10_cfg_save();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "status", 1);
    cJSON_AddStringToObject(o, "msg", "ok");
    return send_json(req, o);
}

static esp_err_t calibrate_awb(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    char *body = recv_body(req);
    cJSON *in = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!in)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    const char *name = NULL;
    const char *bad = json_name(in, &name);
    if (bad) {
        cJSON_Delete(in);
        return reject_cfg(req, bad);
    }
    const char *why = hp10_sky_awb_cal_begin(name);
    cJSON_Delete(in);
    if (why)
        return reject_cfg(req, why);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "status", 1);
    cJSON_AddStringToObject(o, "msg", "Calibrating. Keep the camera still.");
    return send_json(req, o);
}

static esp_err_t delete_awb_preset(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    if (hp10_sky_awb_cal_busy())
        return reject_cfg(req, "Calibration is running.");
    char *body = recv_body(req);
    cJSON *in = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!in)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);

    int slot;
    bool ok = json_int(in, "slot", &slot);
    cJSON_Delete(in);
    if (!ok)
        return reject_cfg(req, "missing preset");
    const char *why = hp10_sky_awb_preset_delete(slot);
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
    register_get(http, "/get_awb", get_awb);
    register_post(http, "/set_sky_cfg", set_sky_cfg);
    register_post(http, "/set_awb", set_awb);
    register_post(http, "/save_awb_preset", save_awb_preset);
    register_post(http, "/calibrate_awb", calibrate_awb);
    register_post(http, "/delete_awb_preset", delete_awb_preset);
}
