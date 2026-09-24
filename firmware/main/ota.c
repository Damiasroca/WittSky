/* Configurable OTA. Check URL is user-set; JSON matches stock:
 * { "code": 0, "data": { "version", "content", "attach1file" } }
 */

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hp10_bringup.h"
#include "pins.h"

static const char *TAG = "ota";

#define FW_URL_MAX 256
#define RESP_CAP   2048

typedef struct {
    char   *buf;
    size_t  n;
    size_t  cap;
} resp_acc_t;

static char     s_fw_url[FW_URL_MAX];
static char     s_ver[32];
static char     s_notes[256];
static char     s_msg[320];
static bool     s_has_update;
static volatile bool     s_busy;
static volatile bool     s_fail;
static volatile bool     s_ok;
static volatile uint32_t s_done;
static volatile uint32_t s_total;
static esp_ota_handle_t s_local;
static const esp_partition_t *s_part;
static bool s_hdr_ok;

bool hp10_ota_url_ok(const char *url)
{
    if (!url || !url[0])
        return false;
    if (strlen(url) >= HP10_UPLOAD_URL_MAX)
        return false;
    return strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0;
}

bool hp10_ota_has_update(void)
{
    return s_has_update;
}

bool hp10_ota_busy(void)
{
    return s_busy;
}

bool hp10_ota_failed(void)
{
    return s_fail;
}

bool hp10_ota_ok(void)
{
    return s_ok;
}

const char *hp10_ota_msg(void)
{
    if (!s_msg[0])
        snprintf(s_msg, sizeof s_msg, "Current version: %s", HP10_VERSION);
    return s_msg;
}

unsigned hp10_ota_pct(void)
{
    if (!s_total)
        return s_busy ? 1 : 0;
    unsigned p = (unsigned)((s_done * 100u) / s_total);
    if (p > 100)
        p = 100;
    return p;
}

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    if (e->event_id != HTTP_EVENT_ON_DATA || !e->data || e->data_len <= 0)
        return ESP_OK;
    resp_acc_t *a = e->user_data;
    if (!a || !a->buf)
        return ESP_OK;
    size_t room = a->cap - a->n - 1;
    size_t take = (size_t)e->data_len < room ? (size_t)e->data_len : room;
    memcpy(a->buf + a->n, e->data, take);
    a->n += take;
    a->buf[a->n] = 0;
    return ESP_OK;
}

static int json_code(cJSON *code)
{
    if (!code)
        return -1;
    if (cJSON_IsNumber(code))
        return code->valueint;
    if (cJSON_IsString(code) && code->valuestring)
        return atoi(code->valuestring);
    return -1;
}

int hp10_ota_check(void)
{
    s_has_update = false;
    s_fw_url[0] = 0;
    s_ver[0] = 0;
    s_notes[0] = 0;
    snprintf(s_msg, sizeof s_msg, "Current version: %s", HP10_VERSION);

    if (!hp10_ota_url_ok(g_ota_url)) {
        snprintf(s_msg, sizeof s_msg, "Set an OTA URL");
        return -1;
    }
    if (!hp10_sta_has_ip()) {
        snprintf(s_msg, sizeof s_msg, "Waiting for router connection");
        return -1;
    }

    char *resp = calloc(1, RESP_CAP);
    if (!resp)
        return -1;
    resp_acc_t acc = { .buf = resp, .n = 0, .cap = RESP_CAP };
    bool https = strncmp(g_ota_url, "https://", 8) == 0;
    esp_http_client_config_t cfg = {
        .url = g_ota_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .crt_bundle_attach = https ? esp_crt_bundle_attach : NULL,
        .event_handler = http_evt,
        .user_data = &acc,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        free(resp);
        snprintf(s_msg, sizeof s_msg, "OTA check failed");
        return -1;
    }
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    if (err != ESP_OK || status < 200 || status > 299 || !acc.n) {
        snprintf(s_msg, sizeof s_msg, "OTA check failed (%s %d)",
                 esp_err_to_name(err), status);
        free(resp);
        return -1;
    }

    const char *brace = strchr(resp, '{');
    cJSON *o = cJSON_Parse(brace ? brace : resp);
    free(resp);
    if (!o) {
        snprintf(s_msg, sizeof s_msg, "OTA JSON invalid");
        return -1;
    }
    cJSON *data = cJSON_GetObjectItem(o, "data");
    if (json_code(cJSON_GetObjectItem(o, "code")) != 0 || !data) {
        cJSON_Delete(o);
        snprintf(s_msg, sizeof s_msg, "Current version: %s", HP10_VERSION);
        return 0;
    }
    cJSON *it = cJSON_GetObjectItem(data, "version");
    if (it && cJSON_IsString(it) && it->valuestring)
        strlcpy(s_ver, it->valuestring, sizeof s_ver);
    it = cJSON_GetObjectItem(data, "content");
    if (it && cJSON_IsString(it) && it->valuestring)
        strlcpy(s_notes, it->valuestring, sizeof s_notes);
    it = cJSON_GetObjectItem(data, "attach1file");
    if (it && cJSON_IsString(it) && it->valuestring)
        strlcpy(s_fw_url, it->valuestring, sizeof s_fw_url);
    cJSON_Delete(o);

    if (!s_fw_url[0] || (strncmp(s_fw_url, "http://", 7) &&
                         strncmp(s_fw_url, "https://", 8))) {
        snprintf(s_msg, sizeof s_msg, "OTA reply has no firmware URL");
        return -1;
    }
    if (!s_ver[0] || strcmp(s_ver, HP10_VERSION) == 0) {
        snprintf(s_msg, sizeof s_msg, "Current version: %s", HP10_VERSION);
        return 0;
    }
    s_has_update = true;
    if (s_notes[0])
        snprintf(s_msg, sizeof s_msg, "New Version: %s\n%s", s_ver, s_notes);
    else
        snprintf(s_msg, sizeof s_msg, "New Version: %s", s_ver);
    ESP_LOGI(TAG, "update %s -> %s", HP10_VERSION, s_fw_url);
    return 1;
}

static void ota_task(void *arg)
{
    (void)arg;
    bool https = strncmp(s_fw_url, "https://", 8) == 0;
    esp_http_client_config_t http = {
        .url = s_fw_url,
        .timeout_ms = 60000,
        .crt_bundle_attach = https ? esp_crt_bundle_attach : NULL,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };
    esp_https_ota_handle_t h = NULL;
    if (esp_https_ota_begin(&cfg, &h) != ESP_OK) {
        ESP_LOGE(TAG, "begin failed");
        s_fail = true;
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }
    esp_app_desc_t desc;
    if (esp_https_ota_get_img_desc(h, &desc) != ESP_OK) {
        ESP_LOGE(TAG, "bad image header");
        esp_https_ota_abort(h);
        s_fail = true;
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "image %s", desc.version);
    int sz = esp_https_ota_get_image_size(h);
    if (sz > 0)
        s_total = (uint32_t)sz;

    esp_err_t err;
    do {
        err = esp_https_ota_perform(h);
        int done = esp_https_ota_get_image_len_read(h);
        if (done > 0)
            s_done = (uint32_t)done;
        if (s_total == 0 && done > 0)
            s_total = (uint32_t)done + 1;
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA fail %s", esp_err_to_name(err));
        esp_https_ota_abort(h);
        s_fail = true;
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }
    if (esp_https_ota_finish(h) != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish fail");
        s_fail = true;
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }
    s_done = s_total ? s_total : s_done;
    s_ok = true;
    ESP_LOGI(TAG, "OTA ok, reboot");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

int hp10_ota_start(void)
{
    if (s_busy)
        return 0;
    if (!s_has_update || !s_fw_url[0])
        return -1;
    if (!hp10_sta_has_ip())
        return -1;
    s_fail = false;
    s_ok = false;
    s_done = 0;
    s_total = 0;
    s_busy = true;
    if (xTaskCreate(ota_task, "http_ota", 10240, NULL, 7, NULL) != pdPASS) {
        s_busy = false;
        return 0;
    }
    return 1;
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

void hp10_ota_reboot_soon(void)
{
    xTaskCreate(reboot_task, "ota_rb", 2048, NULL, 5, NULL);
}

esp_err_t hp10_ota_local_begin(size_t total)
{
    if (s_busy) {
        snprintf(s_msg, sizeof s_msg, "upgrade task is going on...");
        return ESP_ERR_INVALID_STATE;
    }
    s_part = esp_ota_get_next_update_partition(NULL);
    if (!s_part) {
        snprintf(s_msg, sizeof s_msg, "No OTA slot");
        return ESP_FAIL;
    }
    if (total < 0x200 || total > s_part->size) {
        snprintf(s_msg, sizeof s_msg, "Bad firmware size");
        return ESP_ERR_INVALID_SIZE;
    }
    s_fail = false;
    s_ok = false;
    s_done = 0;
    s_total = (uint32_t)total;
    s_local = 0;
    s_hdr_ok = false;
    s_busy = true;
    snprintf(s_msg, sizeof s_msg, "Flashing from PC");
    return ESP_OK;
}

esp_err_t hp10_ota_local_write(const void *data, size_t n)
{
    if (!s_busy || s_fail || !data || !n)
        return ESP_FAIL;
    const uint8_t *p = data;
    if (!s_hdr_ok) {
        if (n >= 4 && p[0] == 0x7F && p[1] == 'E' && p[2] == 'L' && p[3] == 'F') {
            snprintf(s_msg, sizeof s_msg, "That is an ELF, not an app .bin");
            s_fail = true;
            s_busy = false;
            return ESP_ERR_OTA_VALIDATE_FAILED;
        }
        if (p[0] != 0xE9) {
            snprintf(s_msg, sizeof s_msg, "Not an ESP32 app image");
            s_fail = true;
            s_busy = false;
            return ESP_ERR_OTA_VALIDATE_FAILED;
        }
        esp_err_t e = esp_ota_begin(s_part, s_total, &s_local);
        if (e != ESP_OK) {
            snprintf(s_msg, sizeof s_msg, "OTA begin failed");
            s_fail = true;
            s_busy = false;
            return e;
        }
        s_hdr_ok = true;
    }
    esp_err_t e = esp_ota_write(s_local, data, n);
    if (e != ESP_OK) {
        snprintf(s_msg, sizeof s_msg, "Flash write failed");
        esp_ota_abort(s_local);
        s_local = 0;
        s_fail = true;
        s_busy = false;
        return e;
    }
    s_done += (uint32_t)n;
    return ESP_OK;
}

esp_err_t hp10_ota_local_finish(void)
{
    if (!s_busy || !s_hdr_ok || s_fail) {
        hp10_ota_local_abort();
        return ESP_FAIL;
    }
    if (s_done != s_total) {
        snprintf(s_msg, sizeof s_msg, "Upload truncated");
        hp10_ota_local_abort();
        return ESP_FAIL;
    }
    esp_err_t e = esp_ota_end(s_local);
    s_local = 0;
    if (e != ESP_OK) {
        snprintf(s_msg, sizeof s_msg, "Firmware verify failed");
        s_fail = true;
        s_busy = false;
        s_hdr_ok = false;
        return e;
    }
    e = esp_ota_set_boot_partition(s_part);
    if (e != ESP_OK) {
        snprintf(s_msg, sizeof s_msg, "Could not select new slot");
        s_fail = true;
        s_busy = false;
        s_hdr_ok = false;
        return e;
    }
    s_ok = true;
    snprintf(s_msg, sizeof s_msg, "Flash from PC ok");
    ESP_LOGI(TAG, "local OTA ok, reboot");
    return ESP_OK;
}

void hp10_ota_local_abort(void)
{
    if (s_local) {
        esp_ota_abort(s_local);
        s_local = 0;
    }
    if (s_busy && !s_ok)
        s_fail = true;
    s_busy = false;
    s_hdr_ok = false;
}
