#include "cJSON.h"
#include "esp_camera.h"
#include "esp_log.h"

#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include "pins.h"
#include "sky_json.h"

static const char *TAG = "sky";

static void add_dec(cJSON *o, const char *key, double v, int digits)
{
    char buf[24];
    snprintf(buf, sizeof buf, "%.*f", digits, v);
    cJSON_AddRawToObject(o, key, buf);
}

static void add_opt(cJSON *o, const char *key, bool ok, double v, int digits)
{
    if (ok)
        add_dec(o, key, v, digits);
    else
        cJSON_AddNullToObject(o, key);
}

static void add_pair(cJSON *o, const char *key, int a, int b)
{
    cJSON *arr = cJSON_AddArrayToObject(o, key);
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(a));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(b));
}

static void image_levels(int *bri, int *con, int *sat, int *fx)
{
    *bri = 0;
    *con = 0;
    *sat = 0;
    *fx = 0;
    sensor_t *cam = esp_camera_sensor_get();
    if (!cam)
        return;
    *bri = cam->status.brightness;
    *con = cam->status.contrast;
    *sat = cam->status.saturation;
    *fx = cam->status.special_effect;
}

static void add_gains(cJSON *o, const sky_sensor_t *sensor)
{
    if (!sensor->gains_ok) {
        cJSON_AddNullToObject(o, "awb_gains");
        return;
    }
    cJSON *arr = cJSON_AddArrayToObject(o, "awb_gains");
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(sensor->awb_r));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(sensor->awb_g));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(sensor->awb_b));
}

char *sky_format_json(uint16_t frame_w, uint16_t frame_h,
                      uint16_t dec_w, uint16_t dec_h,
                      const sky_metric_in_t *in, const sky_metric_out_t *m,
                      const sky_sensor_t *sensor, int day, int ms)
{
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    time_t now = 0;
    time(&now);
    if (now < 1600000000)
        now = 0;

    cJSON_AddNumberToObject(o, "v", 1);
    cJSON_AddNumberToObject(o, "ts", (double)now);
    cJSON_AddStringToObject(o, "fw", HP10_VERSION);
    add_pair(o, "frame", frame_w, frame_h);
    add_pair(o, "dec", dec_w, dec_h);
    add_dec(o, "luma", m->luma, 1);
    if (sensor->read_ok) {
        cJSON_AddNumberToObject(o, "aec", sensor->aec);
        cJSON_AddNumberToObject(o, "gain_reg", sensor->gain_reg);
        add_dec(o, "gain_x", sensor->gain_x, 2);
        add_opt(o, "light_idx", sensor->light_ok, sensor->light_idx, 2);
    } else {
        cJSON_AddNullToObject(o, "aec");
        cJSON_AddNullToObject(o, "gain_reg");
        cJSON_AddNullToObject(o, "gain_x");
        cJSON_AddNullToObject(o, "light_idx");
    }
    cJSON_AddNumberToObject(o, "awb_mode", sensor->awb_mode);
    add_gains(o, sensor);
    int bri = 0, con = 0, sat = 0, fx = 0;
    image_levels(&bri, &con, &sat, &fx);
    cJSON *img = cJSON_AddObjectToObject(o, "img");
    cJSON_AddNumberToObject(img, "bri", bri);
    cJSON_AddNumberToObject(img, "con", con);
    cJSON_AddNumberToObject(img, "sat", sat);
    cJSON_AddNumberToObject(img, "fx", fx);
    cJSON_AddBoolToObject(o, "img_default",
                          bri == 0 && con == 0 && sat == 0 && fx == 0);
    if (day < 0)
        cJSON_AddNullToObject(o, "day");
    else
        cJSON_AddBoolToObject(o, "day", day > 0);

    cJSON *mask = cJSON_AddArrayToObject(o, "mask");
    cJSON_AddItemToArray(mask, cJSON_CreateNumber(in->x));
    cJSON_AddItemToArray(mask, cJSON_CreateNumber(in->y));
    cJSON_AddItemToArray(mask, cJSON_CreateNumber(in->w));
    cJSON_AddItemToArray(mask, cJSON_CreateNumber(in->h));
    cJSON_AddNumberToObject(o, "sky_px", m->sky_px);

    if (fx != 0)
        ESP_LOGW(TAG, "special effect %d, cloud fields cleared", fx);
    bool cloud = fx == 0 && day != 0 && m->cloud_ok && m->sky_px > 0 && m->usable_n > 0;
    double sat_pct = 0, cloud_pct = 0, rb_mean = 0;
    if (cloud) {
        sat_pct = 100.0 * (double)m->sat_n / (double)m->sky_px;
        cloud_pct = 100.0 * (double)m->cloud_n / (double)m->usable_n;
        if (m->rb_n > 0)
            rb_mean = m->rb_sum / (double)m->rb_n;
    }
    add_opt(o, "sat_pct", cloud, sat_pct, 1);
    add_opt(o, "cloud_pct", cloud, cloud_pct, 1);
    add_opt(o, "rb_mean", cloud && m->rb_n > 0, rb_mean, 3);
    add_opt(o, "sharp", m->sharp_ok, m->sharp, 1);
    cJSON_AddNumberToObject(o, "ms", ms);

    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}
