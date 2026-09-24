#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdlib.h>
#include <string.h>

#include "hp10_bringup.h"
#include "sky_cfg.h"
#include "sky_json.h"
#include "sky_metrics.h"
#include "sky_sensor.h"
#include "sky_stats.h"

static const char *TAG = "sky";

#define SKY_JSON_MAX HP10_SKY_JSON_MAX
#define SKY_RGB_MAX  (256 * 1024)

static StaticSemaphore_t s_mu_buf;
static SemaphoreHandle_t s_mu;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_json[SKY_JSON_MAX];
static bool s_valid;

static void sky_lock(void)
{
    if (!s_mu) {
        portENTER_CRITICAL(&s_init_mux);
        if (!s_mu)
            s_mu = xSemaphoreCreateMutexStatic(&s_mu_buf);
        portEXIT_CRITICAL(&s_init_mux);
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
}

static void sky_unlock(void)
{
    xSemaphoreGive(s_mu);
}

static void *sky_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p)
        p = malloc(n);
    return p;
}

static esp_err_t decode_rgb(const uint8_t *jpeg, size_t jpeg_len,
                            uint8_t **rgb, uint16_t *jpg_w, uint16_t *jpg_h,
                            uint16_t *dec_w, uint16_t *dec_h)
{
    esp_jpeg_image_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.indata = (uint8_t *)jpeg;
    cfg.indata_size = (uint32_t)jpeg_len;
    cfg.out_format = JPEG_IMAGE_FORMAT_RGB888;
    cfg.out_scale = JPEG_IMAGE_SCALE_1_8;

    esp_jpeg_image_output_t info;
    memset(&info, 0, sizeof info);
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK || info.output_len == 0 ||
        info.output_len > SKY_RGB_MAX || info.width == 0 || info.height == 0) {
        ESP_LOGE(TAG, "jpeg info failed");
        return ESP_FAIL;
    }

    uint8_t *buf = sky_alloc(info.output_len);
    if (!buf) {
        ESP_LOGE(TAG, "rgb alloc %u failed", (unsigned)info.output_len);
        return ESP_ERR_NO_MEM;
    }
    cfg.outbuf = buf;
    cfg.outbuf_size = (uint32_t)info.output_len;

    esp_jpeg_image_output_t dec;
    memset(&dec, 0, sizeof dec);
    if (esp_jpeg_decode(&cfg, &dec) != ESP_OK || dec.width == 0 || dec.height == 0) {
        ESP_LOGE(TAG, "jpeg decode failed");
        free(buf);
        return ESP_FAIL;
    }

    *rgb = buf;
    *jpg_w = info.width;
    *jpg_h = info.height;
    *dec_w = dec.width;
    *dec_h = dec.height;
    return ESP_OK;
}

esp_err_t hp10_sky_compute(const uint8_t *jpeg, size_t jpeg_len,
                           uint16_t frame_w, uint16_t frame_h,
                           char *json_out, size_t json_out_n)
{
    if (!jpeg || jpeg_len < 4)
        return ESP_ERR_INVALID_ARG;
    if (!g_sky_en)
        return ESP_ERR_INVALID_STATE;

    sky_metric_in_t in = {
        .x = g_sky_x,
        .y = g_sky_y,
        .w = g_sky_w,
        .h = g_sky_h,
        .sat_thr = g_sky_sat,
        .rb_x1000 = g_sky_rb,
    };

    int64_t t0 = esp_timer_get_time();
    uint8_t *rgb = NULL;
    uint16_t jpg_w = 0, jpg_h = 0, dec_w = 0, dec_h = 0;
    esp_err_t err = decode_rgb(jpeg, jpeg_len, &rgb, &jpg_w, &jpg_h, &dec_w, &dec_h);
    if (err != ESP_OK)
        return err;

    size_t pix = (size_t)dec_w * (size_t)dec_h;
    uint8_t *gray = sky_alloc(pix);
    if (!gray) {
        ESP_LOGE(TAG, "gray alloc %u failed", (unsigned)pix);
        free(rgb);
        return ESP_ERR_NO_MEM;
    }
    sky_metric_out_t measured;
    memset(&measured, 0, sizeof measured);
    if (!sky_measure(rgb, dec_w, dec_h, &in, gray, &measured)) {
        free(rgb);
        free(gray);
        return ESP_FAIL;
    }
    free(rgb);
    free(gray);

    if (!measured.cloud_ok) {
        if (measured.sky_px == 0)
            ESP_LOGW(TAG, "sky mask empty, cloud fields cleared");
        else
            ESP_LOGW(TAG, "sky usable %u/%u below 5%%, cloud fields cleared",
                     (unsigned)measured.usable_n, (unsigned)measured.sky_px);
    }

    sky_sensor_t sensor;
    sky_sensor_read(measured.luma, &sensor);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (ms < 0)
        ms = 0;
    uint16_t fw = frame_w ? frame_w : jpg_w;
    uint16_t fh = frame_h ? frame_h : jpg_h;
    int day = -1;
    int now_m = 0, rise_m = 0, set_m = 0;
    if (hp10_sun_minutes(&now_m, &rise_m, &set_m)) {
        int begin = rise_m + (int)g_sky_daym;
        int end = set_m - (int)g_sky_daym;
        day = (now_m >= begin && now_m <= end) ? 1 : 0;
    }
    char *text = sky_format_json(fw, fh, dec_w, dec_h, &in, &measured, &sensor, day, ms);
    if (!text)
        return ESP_ERR_NO_MEM;
    size_t n = strlen(text);
    if (n + 1 > SKY_JSON_MAX) {
        ESP_LOGE(TAG, "json %u too long", (unsigned)n);
        free(text);
        return ESP_FAIL;
    }
    if (n > 400)
        ESP_LOGW(TAG, "json %u bytes", (unsigned)n);

    sky_lock();
    memcpy(s_json, text, n + 1);
    s_valid = true;
    sky_unlock();
    ESP_LOGI(TAG, "%s", text);

    err = ESP_OK;
    if (json_out) {
        if (json_out_n <= n) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            memcpy(json_out, text, n + 1);
        }
    }
    free(text);
    return err;
}

bool hp10_sky_latest(char *dst, size_t n)
{
    if (!dst || n == 0)
        return false;
    sky_lock();
    bool ok = s_valid && strlen(s_json) + 1 <= n;
    if (ok)
        memcpy(dst, s_json, strlen(s_json) + 1);
    else
        dst[0] = 0;
    sky_unlock();
    return ok;
}
