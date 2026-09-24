#include "esp_http_client.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mbedtls/md.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hp10_bringup.h"
#include "pins.h"
#include "upload_priv.h"

static const char *TAG = "upload";

#define BOUNDARY_TAG "----DataPackageBoundary"
#define ECO_STATION  HP10_VERSION
#define ECO_MODEL    "HP10"

static uint8_t s_reupload;
static uint8_t s_eco_host;

static int md5_hex(const uint8_t *data, size_t n, char out[33])
{
    uint8_t dig[16];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    if (!info || mbedtls_md(info, data, n, dig) != 0)
        return -1;
    for (int i = 0; i < 16; i++)
        sprintf(out + 2 * i, "%02X", dig[i]);
    return 0;
}

static int append_str(char *dst, size_t cap, size_t *n, const char *s)
{
    size_t len = strlen(s);
    if (*n + len >= cap)
        return -1;
    memcpy(dst + *n, s, len);
    *n += len;
    dst[*n] = 0;
    return 0;
}

static int append_part(char *dst, size_t cap, size_t *n,
                       const char *name, const char *val)
{
    char line[192];
    snprintf(line, sizeof line,
             "--" BOUNDARY_TAG "\r\n"
             "Content-Disposition: form-data; name=\"%s\"\r\n\r\n"
             "%s\r\n", name, val);
    return append_str(dst, cap, n, line);
}

static int eco_errcode(const char *resp)
{
    const char *p = strstr(resp, "errcode");
    if (!p)
        return -1;
    p += 7;
    while (*p && (*p == '"' || *p == ':' || *p == ' '))
        p++;
    return atoi(p);
}

static esp_err_t upload_ecowitt_url(const char *url, const char *body, int body_n)
{
    http_acc_t acc = {0};
    ESP_LOGI(TAG, "ecowitt POST %s body=%d", url, body_n);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .event_handler = http_evt,
        .user_data = &acc,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        ESP_LOGE(TAG, "ecowitt http client init failed");
        return ESP_FAIL;
    }

    esp_http_client_set_header(cli, "Content-Type",
                               "multipart/form-data; boundary=" BOUNDARY_TAG);
    esp_http_client_set_header(cli, "Accept", "*/*");
    esp_http_client_set_post_field(cli, body, body_n);
    esp_err_t err = esp_http_client_perform(cli);
    log_http_result("ecowitt", url, cli, err, &acc);
    int status = esp_http_client_get_status_code(cli);
    int code = acc.n ? eco_errcode(acc.buf) : -1;
    ESP_LOGI(TAG, "ecowitt errcode=%d", code);
    if (err == ESP_OK && (status < 200 || status > 299))
        err = ESP_FAIL;
    if (err == ESP_OK && code > 0)
        err = ESP_FAIL;
    esp_http_client_cleanup(cli);
    return err;
}

esp_err_t upload_ecowitt(void)
{
    ESP_LOGI(TAG, "ecowitt start station=%s model=%s", ECO_STATION, ECO_MODEL);
    log_sta();
    if (!want_ecowitt()) {
        ESP_LOGW(TAG, "ecowitt abort: not selected");
        return ESP_ERR_INVALID_STATE;
    }
    if (!hp10_sta_has_ip()) {
        ESP_LOGW(TAG, "ecowitt abort: no STA IP");
        return ESP_ERR_INVALID_STATE;
    }

    camera_fb_t *fb = grab_frame();
    if (!fb) {
        hp10_upload_result_set(false, "camera grab failed");
        return ESP_FAIL;
    }
    sky_stats_on_frame(fb, NULL, 0);

    time_t now = time(NULL);
    if (now < 1600000000) {
        ESP_LOGW(TAG, "clock not synced unix=%ld — Ecowitt may reject dateutc",
                 (long)now);
        now = 0;
    }
    struct tm tm;
    gmtime_r(&now, &tm);
    char dateutc[32];
    strftime(dateutc, sizeof dateutc, "%Y-%m-%d %H:%M:%S", &tm);

    char mac[18], passkey[33], jpeg_md5[33], tmp[48], img_hdr[192];
    snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x",
             g_abStaMac[0], g_abStaMac[1], g_abStaMac[2],
             g_abStaMac[3], g_abStaMac[4], g_abStaMac[5]);
    char mac_up[18];
    snprintf(mac_up, sizeof mac_up, "%02X:%02X:%02X:%02X:%02X:%02X",
             g_abStaMac[0], g_abStaMac[1], g_abStaMac[2],
             g_abStaMac[3], g_abStaMac[4], g_abStaMac[5]);
    if (md5_hex((const uint8_t *)mac_up, strlen(mac_up), passkey) != 0 ||
        md5_hex(fb->buf, fb->len, jpeg_md5) != 0) {
        ESP_LOGE(TAG, "ecowitt md5 failed");
        drop_frame(fb);
        return ESP_FAIL;
    }

    s_reupload++;
    ESP_LOGI(TAG, "ecowitt mac=%s PASSKEY=%s dateutc='%s' interval=%u reupload=%u jpeg_md5=%s",
             mac, passkey, dateutc, (unsigned)g_ost_interval * 5,
             (unsigned)s_reupload, jpeg_md5);

    size_t cap = fb->len + 2048;
    char *pkt = psram_malloc(cap);
    if (!pkt) {
        ESP_LOGE(TAG, "ecowitt malloc %u failed", (unsigned)cap);
        drop_frame(fb);
        return ESP_ERR_NO_MEM;
    }

    size_t n = 0;
    int bad = 0;
    pkt[0] = 0;
    bad |= append_part(pkt, cap, &n, "mac", mac);
    bad |= append_part(pkt, cap, &n, "PASSKEY", passkey);
    bad |= append_part(pkt, cap, &n, "stationtype", ECO_STATION);
    bad |= append_part(pkt, cap, &n, "model", ECO_MODEL);
    bad |= append_part(pkt, cap, &n, "dateutc", dateutc);
    bad |= append_part(pkt, cap, &n, "battery", "0");
    snprintf(tmp, sizeof tmp, "%u", (unsigned)s_reupload);
    bad |= append_part(pkt, cap, &n, "reupload", tmp);
    snprintf(tmp, sizeof tmp, "%u", (unsigned)g_ost_interval * 5);
    bad |= append_part(pkt, cap, &n, "interval", tmp);
    bad |= append_part(pkt, cap, &n, "md5", jpeg_md5);
    if (bad) {
        ESP_LOGE(TAG, "ecowitt multipart overflow");
        free(pkt);
        drop_frame(fb);
        return ESP_ERR_NO_MEM;
    }

    snprintf(img_hdr, sizeof img_hdr,
             "--" BOUNDARY_TAG "\r\n"
             "Content-Disposition: form-data; name=\"weather_image\";"
             " filename=\"%lu.jpg\"\r\n"
             "Content-Type: image/jpeg\r\n\r\n",
             (unsigned long)now);
    if (append_str(pkt, cap, &n, img_hdr) != 0 || n + fb->len + 48 >= cap) {
        ESP_LOGE(TAG, "ecowitt image header overflow cap=%u n=%u jpeg=%u",
                 (unsigned)cap, (unsigned)n, (unsigned)fb->len);
        free(pkt);
        drop_frame(fb);
        return ESP_ERR_NO_MEM;
    }
    memcpy(pkt + n, fb->buf, fb->len);
    n += fb->len;
    append_str(pkt, cap, &n, "\r\n--" BOUNDARY_TAG "--\r\n");
    drop_frame(fb);
    ESP_LOGI(TAG, "ecowitt packet %u bytes", (unsigned)n);

    static const char *hosts[] = {
        "http://rtpmedia.ecowitt.net/data/upload_image",
        "http://cdnrtpmedia.ecowitt.net/data/upload_image",
    };
    const char *url = hosts[s_eco_host & 1];
    esp_err_t err = upload_ecowitt_url(url, pkt, (int)n);
    if (err != ESP_OK) {
        s_eco_host ^= 1;
        ESP_LOGW(TAG, "ecowitt retry other host");
        err = upload_ecowitt_url(hosts[s_eco_host & 1], pkt, (int)n);
    }
    if (err != ESP_OK && hp10_sta_has_ip()) {
        ESP_LOGW(TAG, "ecowitt retry after pause");
        vTaskDelay(pdMS_TO_TICKS(2000));
        err = upload_ecowitt_url(hosts[s_eco_host & 1], pkt, (int)n);
    }
    free(pkt);
    ESP_LOGI(TAG, "ecowitt done %s", esp_err_to_name(err));
    if (err == ESP_OK) {
        hp10_upload_result_set(true, "ecowitt ok");
        hp10_wd_note_upload_ok();
    } else if (!hp10_sta_has_ip()) {
        hp10_upload_result_set(false, "no STA IP");
    } else {
        char msg[64];
        snprintf(msg, sizeof msg, "ecowitt %s", esp_err_to_name(err));
        hp10_upload_result_set(false, msg);
        hp10_wd_note_upload_fail("upload");
    }
    return err;
}
