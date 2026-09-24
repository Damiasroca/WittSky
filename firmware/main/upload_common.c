#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdlib.h>
#include <string.h>

#include "hp10_bringup.h"
#include "sky_cfg.h"
#include "sky_stats.h"
#include "upload_priv.h"

static const char *TAG = "upload";

bool want_custom(void)
{
    return g_upload_en && !g_ecowitt_en && hp10_upload_url_ok(g_upload_url);
}

bool want_ecowitt(void)
{
    return g_ecowitt_en && !g_upload_en;
}

const char *dest_name(void)
{
    if (want_ecowitt())
        return "ecowitt";
    if (want_custom())
        return "custom";
    return "none";
}

void log_sta(void)
{
    if (!g_pNetifSta) {
        ESP_LOGW(TAG, "STA netif is null");
        return;
    }
    esp_netif_ip_info_t ip = {0};
    if (esp_netif_get_ip_info(g_pNetifSta, &ip) != ESP_OK) {
        ESP_LOGW(TAG, "STA ip query failed");
        return;
    }
    ESP_LOGI(TAG, "STA ip=" IPSTR " gw=" IPSTR " mask=" IPSTR,
             IP2STR(&ip.ip), IP2STR(&ip.gw), IP2STR(&ip.netmask));
}

void log_cfg(const char *when)
{
    ESP_LOGI(TAG, "%s dest=%s eco=%d custom=%d ost=%u (%u min) url='%s' cam=%d sta=%d",
             when, dest_name(), g_ecowitt_en ? 1 : 0, g_upload_en ? 1 : 0,
             g_ost_interval, (unsigned)g_ost_interval * 5,
             g_upload_url[0] ? g_upload_url : "",
             g_bCameraOk ? 1 : 0, hp10_sta_has_ip() ? 1 : 0);
}

static camera_fb_t *grab_once(void)
{
    xSemaphoreTake(g_cam_mu, portMAX_DELAY);
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb)
        xSemaphoreGive(g_cam_mu);
    return fb;
}

camera_fb_t *grab_frame(void)
{
    if (!g_cam_mu) {
        ESP_LOGE(TAG, "grab: camera mutex missing");
        return NULL;
    }
    if (!g_bCameraOk) {
        esp_err_t err = hp10_camera_ensure();
        if (err != ESP_OK) {
            if (err != ESP_ERR_INVALID_STATE)
                hp10_wd_note_camera_fail("camera_init");
            ESP_LOGE(TAG, "grab: camera down");
            return NULL;
        }
    }
    camera_fb_t *fb = grab_once();
    if (!fb) {
        ESP_LOGE(TAG, "grab: esp_camera_fb_get returned null");
        esp_err_t rc = hp10_camera_recover();
        if (rc == ESP_OK)
            fb = grab_once();
        else if (rc == ESP_ERR_INVALID_STATE)
            return NULL;
    }
    if (!fb) {
        hp10_wd_note_camera_fail("camera_grab");
        return NULL;
    }
    hp10_wd_note_camera_ok();
    ESP_LOGI(TAG, "grab: jpeg %u bytes %ux%u fmt=%d",
             (unsigned)fb->len, fb->width, fb->height, (int)fb->format);
    return fb;
}

void drop_frame(camera_fb_t *fb)
{
    if (fb)
        esp_camera_fb_return(fb);
    if (g_cam_mu)
        xSemaphoreGive(g_cam_mu);
}

void sky_stats_on_frame(const camera_fb_t *fb, char *json, size_t json_n)
{
    if (json && json_n)
        json[0] = 0;
    if (!g_sky_en || !fb)
        return;
    hp10_sky_compute(fb->buf, fb->len, (uint16_t)fb->width, (uint16_t)fb->height,
                     json, json_n);
}

static void scrub_log(char *s)
{
    for (; s && *s; s++) {
        if (*s == '\r' || *s == '\n' || *s == '\t')
            *s = ' ';
    }
}

esp_err_t http_evt(esp_http_client_event_t *evt)
{
    http_acc_t *a = evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR: {
        int tls_code = 0, tls_flags = 0;
        esp_err_t tls = ESP_FAIL;
        if (evt->client)
            tls = esp_http_client_get_and_clear_last_tls_error(
                evt->client, &tls_code, &tls_flags);
        ESP_LOGE(TAG, "http ERROR tls=%s code=%d flags=0x%x errno=%d",
                 esp_err_to_name(tls), tls_code, tls_flags,
                 evt->client ? esp_http_client_get_errno(evt->client) : -1);
        break;
    }
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(TAG, "http connected");
        break;
    case HTTP_EVENT_HEADERS_SENT:
        ESP_LOGI(TAG, "http headers sent");
        break;
    case HTTP_EVENT_ON_HEADER:
        if (evt->header_key && evt->header_value)
            ESP_LOGI(TAG, "http hdr %s: %s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        if (!a || evt->data_len <= 0)
            break;
        {
            int room = (int)sizeof a->buf - 1 - a->n;
            int take = evt->data_len < room ? evt->data_len : room;
            if (take > 0) {
                memcpy(a->buf + a->n, evt->data, take);
                a->n += take;
                a->buf[a->n] = 0;
            }
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "http finished");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "http disconnected");
        break;
    default:
        break;
    }
    return ESP_OK;
}

void log_http_result(const char *kind, const char *url,
                     esp_http_client_handle_t cli, esp_err_t err,
                     http_acc_t *acc)
{
    int status = cli ? esp_http_client_get_status_code(cli) : 0;
    int clen = cli ? (int)esp_http_client_get_content_length(cli) : -1;
    int eno = cli ? esp_http_client_get_errno(cli) : -1;
    ESP_LOGI(TAG, "%s POST %s -> %s http=%d len=%d errno=%d",
             kind, url, esp_err_to_name(err), status, clen, eno);
    if (acc && acc->n) {
        scrub_log(acc->buf);
        ESP_LOGI(TAG, "%s body (%d): %s", kind, acc->n, acc->buf);
    } else {
        ESP_LOGW(TAG, "%s empty body", kind);
    }
}

void *psram_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p)
        p = malloc(n);
    return p;
}
