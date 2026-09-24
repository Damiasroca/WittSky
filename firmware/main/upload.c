/* JPEG upload. Custom URL and Ecowitt are exclusive; both default off. */

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#include "hp10_bringup.h"
#include "sky_stats.h"
#include "upload_priv.h"

static const char *TAG = "upload";

static SemaphoreHandle_t s_now_sem;
static volatile bool     s_now_req;
static esp_err_t         s_now_err;

static esp_err_t upload_now(void)
{
    ESP_LOGI(TAG, "custom start url='%s'", g_upload_url);
    log_sta();
    if (!want_custom()) {
        ESP_LOGW(TAG, "custom abort: not selected");
        return ESP_ERR_INVALID_STATE;
    }
    if (!hp10_sta_has_ip()) {
        ESP_LOGW(TAG, "custom abort: no STA IP");
        return ESP_ERR_INVALID_STATE;
    }

    camera_fb_t *fb = grab_frame();
    if (!fb) {
        hp10_upload_result_set(false, "camera grab failed");
        return ESP_FAIL;
    }
    char sky[HP10_SKY_JSON_MAX];
    sky_stats_on_frame(fb, sky, sizeof sky);

    bool https = strncmp(g_upload_url, "https://", 8) == 0;
    esp_http_client_config_t cfg = {
        .url = g_upload_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .event_handler = http_evt,
        .crt_bundle_attach = https ? esp_crt_bundle_attach : NULL,
    };
    ESP_LOGI(TAG, "custom jpeg=%u https=%d", (unsigned)fb->len, https ? 1 : 0);
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 2 && err != ESP_OK; attempt++) {
        if (attempt) {
            if (!hp10_sta_has_ip())
                break;
            ESP_LOGW(TAG, "custom retry");
            vTaskDelay(pdMS_TO_TICKS(2000));
            cfg.timeout_ms = 12000;
        }
        http_acc_t acc = {0};
        cfg.user_data = &acc;
        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        if (!cli) {
            ESP_LOGE(TAG, "custom http client init failed");
            err = ESP_FAIL;
            continue;
        }
        esp_http_client_set_header(cli, "Content-Type", "image/jpeg");
        if (sky[0])
            esp_http_client_set_header(cli, "X-Sky-Stats", sky);
        esp_http_client_set_post_field(cli, (char *)fb->buf, fb->len);
        err = esp_http_client_perform(cli);
        log_http_result("custom", g_upload_url, cli, err, &acc);
        int status = esp_http_client_get_status_code(cli);
        if (err == ESP_OK && (status < 200 || status > 299))
            err = ESP_FAIL;
        esp_http_client_cleanup(cli);
    }
    drop_frame(fb);
    ESP_LOGI(TAG, "custom done %s", esp_err_to_name(err));
    if (err == ESP_OK) {
        hp10_upload_result_set(true, "custom ok");
        hp10_wd_note_upload_ok();
    } else if (!hp10_sta_has_ip()) {
        hp10_upload_result_set(false, "no STA IP");
    } else {
        char msg[64];
        snprintf(msg, sizeof msg, "custom %s", esp_err_to_name(err));
        hp10_upload_result_set(false, msg);
        hp10_wd_note_upload_fail("upload");
    }
    return err;
}

static bool upload_armed(uint8_t slot)
{
    uint32_t mins = (uint32_t)slot * 5;
    if (mins == 0)
        return false;
    if (g_ost_interval != slot)
        return false;
    return want_custom() || want_ecowitt();
}

static const char *idle_why(void)
{
    if (g_ost_interval == 0)
        return "interval=off";
    if (g_upload_en && g_ecowitt_en)
        return "both destinations on";
    if (!g_ecowitt_en && !g_upload_en)
        return "upload disabled";
    if (g_upload_en && !hp10_upload_url_ok(g_upload_url))
        return "custom URL invalid";
    if (!want_custom() && !want_ecowitt())
        return "not armed";
    if (!g_bCameraOk)
        return "camera down";
    if (!hp10_sta_has_ip())
        return "no STA IP";
    return "waiting";
}

static bool dest_ready(void)
{
    return want_custom() || want_ecowitt();
}

static void finish_now(esp_err_t err)
{
    if (!s_now_req)
        return;
    s_now_err = err;
    s_now_req = false;
    if (s_now_sem)
        xSemaphoreGive(s_now_sem);
}

static void do_shot(void)
{
    ESP_LOGI(TAG, "shot dest=%s", dest_name());
    esp_err_t shot = ESP_ERR_INVALID_STATE;
    if (want_ecowitt())
        shot = upload_ecowitt();
    else if (want_custom())
        shot = upload_now();
    finish_now(shot);
}

static void upload_task(void *arg)
{
    (void)arg;
    const char *last_why = "";
    bool first = true;
    TickType_t last_idle = 0;
    ESP_LOGI(TAG, "task started");
    log_cfg("boot");
    for (;;) {
        uint8_t slot = g_ost_interval;
        if (!upload_armed(slot)) {
            first = true;
            if (s_now_req) {
                if (!dest_ready()) {
                    hp10_upload_result_set(false, "upload disabled");
                    finish_now(ESP_ERR_INVALID_STATE);
                } else if (!hp10_sta_has_ip()) {
                    hp10_upload_result_set(false, "no STA IP");
                    finish_now(ESP_ERR_INVALID_STATE);
                } else if (!g_bCameraOk) {
                    hp10_upload_result_set(false, "camera down");
                    finish_now(ESP_FAIL);
                } else {
                    do_shot();
                }
                continue;
            }
            const char *why = idle_why();
            TickType_t now = xTaskGetTickCount();
            if (why != last_why || (now - last_idle) > pdMS_TO_TICKS(30000)) {
                ESP_LOGI(TAG, "idle: %s", why);
                log_cfg("idle");
                last_why = why;
                last_idle = now;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        uint32_t mins = (uint32_t)slot * 5;
        if (first) {
            first = false;
            ESP_LOGI(TAG, "armed dest=%s — first shot in 5s, then every %u min",
                     dest_name(), mins);
            log_sta();
            for (int i = 0; i < 25 && !s_now_req; i++)
                vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            ESP_LOGI(TAG, "next %s upload in %u min", dest_name(), mins);
            for (uint32_t i = 0; i < mins * 60; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (!upload_armed(slot) || s_now_req)
                    break;
            }
        }
        if (s_now_req) {
            /* fall through even if interval is off */
        } else if (!upload_armed(slot)) {
            ESP_LOGI(TAG, "disarmed before shot");
            continue;
        }
        if (!hp10_sta_has_ip()) {
            ESP_LOGW(TAG, "skip shot: no STA IP, retry in 10s");
            log_sta();
            first = true;
            if (s_now_req) {
                hp10_upload_result_set(false, "no STA IP");
                finish_now(ESP_ERR_INVALID_STATE);
            }
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        if (!g_bCameraOk) {
            esp_err_t cerr = hp10_camera_ensure();
            if (cerr != ESP_OK) {
                if (cerr != ESP_ERR_INVALID_STATE)
                    hp10_wd_note_camera_fail("camera_init");
                ESP_LOGW(TAG, "skip shot: camera down, retry in 10s");
                first = true;
                if (s_now_req) {
                    hp10_upload_result_set(false, "camera down");
                    finish_now(ESP_FAIL);
                }
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
        }
        do_shot();
        if (!hp10_sta_has_ip()) {
            first = true;
            ESP_LOGW(TAG, "offline after shot, retry when STA is back");
        }
    }
}

esp_err_t hp10_upload_run_now(uint32_t wait_ms)
{
    if (!dest_ready())
        return ESP_ERR_INVALID_STATE;
    if (!s_now_sem)
        return ESP_ERR_INVALID_STATE;
    s_now_err = ESP_FAIL;
    s_now_req = true;
    if (xSemaphoreTake(s_now_sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        s_now_req = false;
        xSemaphoreTake(s_now_sem, 0);
        return ESP_ERR_TIMEOUT;
    }
    return s_now_err;
}

void hp10_upload_start(void)
{
    s_now_sem = xSemaphoreCreateBinary();
    xTaskCreate(upload_task, "upload", 12288, NULL, 3, NULL);
}
