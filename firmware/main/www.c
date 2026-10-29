/* Recovered HP10 web UI + the routes the pages actually call.
 * Capture / stream / video JSON are live. OTA is user-URL + stock JSON.
 */

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

#include "cam_auto.h"
#include "hp10_bringup.h"
#include "pins.h"
#include "www.h"
#include "www_priv.h"

static const char *TAG = "www";

/* Port 80's listen queue fills when its thread is stuck, so a probe to
 * port 80 would time out the same way a browser does and could block this
 * task too. The heartbeat goes through httpd's control socket instead.
 * The thread runs beat_work only when it is idle. If that stamp goes stale
 * for two minutes, the existing watchdog reboots (same cap as camera/upload).
 * A live MJPEG client holds port 81 on purpose, so that server is not watched. */
#define HTTP_BEAT_MS  15000
#define HTTP_STALL_US (120LL * 1000000LL)

static httpd_handle_t  s_http;
static volatile int64_t s_beat_us;
static volatile bool    s_beat_posting;
static TaskHandle_t     s_beat_task;

static volatile bool    s_beat_seen;

static void beat_work(void *arg)
{
    (void)arg;
    s_beat_us = esp_timer_get_time();
    s_beat_seen = true;
}

bool www_alive(void)
{
    return s_beat_seen && esp_timer_get_time() - s_beat_us < 45LL * 1000000LL;
}

/* One long-lived poster instead of a new task every 15 s. If the post
 * ever blocks, only this task is stuck; s_beat_posting stays set and the
 * watch task still sees the stamp go stale. */
static void beat_post_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_http)
            httpd_queue_work(s_http, beat_work, NULL);
        s_beat_posting = false;
    }
}

static void http_watch_task(void *arg)
{
    (void)arg;
    s_beat_us = esp_timer_get_time();
    bool noted = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HTTP_BEAT_MS));
        if (hp10_ota_busy()) {
            s_beat_us = esp_timer_get_time();
            noted = false;
            continue;
        }
        int64_t age = esp_timer_get_time() - s_beat_us;
        if (age > HTTP_STALL_US) {
            if (!noted) {
                noted = true;
                ESP_LOGW(TAG, "port 80 stalled %ds", (int)(age / 1000000));
                hp10_wd_note_http_stall();
            }
            continue;
        }
        noted = false;
        if (!s_beat_posting && s_http && s_beat_task) {
            s_beat_posting = true;
            xTaskNotifyGive(s_beat_task);
        }
    }
}

extern const uint8_t login_html_start[] asm("_binary_login_html_start");
extern const uint8_t login_html_end[]   asm("_binary_login_html_end");
extern const uint8_t video_html_start[] asm("_binary_video_html_start");
extern const uint8_t video_html_end[]   asm("_binary_video_html_end");
extern const uint8_t localNetwork_html_start[] asm("_binary_localNetwork_html_start");
extern const uint8_t localNetwork_html_end[]   asm("_binary_localNetwork_html_end");
extern const uint8_t status_html_start[] asm("_binary_status_html_start");
extern const uint8_t status_html_end[]   asm("_binary_status_html_end");
extern const uint8_t capture_html_start[] asm("_binary_capture_html_start");
extern const uint8_t capture_html_end[]   asm("_binary_capture_html_end");
extern const uint8_t system_html_start[] asm("_binary_system_html_start");
extern const uint8_t system_html_end[]   asm("_binary_system_html_end");
extern const uint8_t skystats_html_start[] asm("_binary_skystats_html_start");
extern const uint8_t skystats_html_end[]   asm("_binary_skystats_html_end");
extern const uint8_t overlay_html_start[] asm("_binary_overlay_html_start");
extern const uint8_t overlay_html_end[]   asm("_binary_overlay_html_end");
extern const uint8_t axcss_css_start[]     asm("_binary_axcss_css_start");
extern const uint8_t axcss_css_end[]       asm("_binary_axcss_css_end");
extern const uint8_t axjs_js_start[] asm("_binary_axjs_js_start");
extern const uint8_t axjs_js_end[]   asm("_binary_axjs_js_end");
extern const uint8_t camera_js_start[] asm("_binary_camera_js_start");
extern const uint8_t camera_js_end[]   asm("_binary_camera_js_end");
extern const uint8_t awb_js_start[] asm("_binary_awb_js_start");
extern const uint8_t awb_js_end[]   asm("_binary_awb_js_end");
extern const uint8_t skymask_js_start[] asm("_binary_skymask_js_start");
extern const uint8_t skymask_js_end[]   asm("_binary_skymask_js_end");
extern const uint8_t expauto_js_start[] asm("_binary_expauto_js_start");
extern const uint8_t expauto_js_end[]   asm("_binary_expauto_js_end");


/* Login sessions. Each browser gets its own random cookie, so one login no
 * longer opens the UI to every client on the network. Only the port 80
 * thread touches this table, so it needs no lock. Reboot logs everyone out. */
#define SESS_N       4
#define SESS_COOKIE  "hp10s"
#define SESS_HEX     32

static char    s_sess[SESS_N][SESS_HEX + 1];
static uint8_t s_sess_next;

bool www_session_ok(httpd_req_t *req)
{
    char val[SESS_HEX + 8];
    size_t n = sizeof val;
    if (httpd_req_get_cookie_val(req, SESS_COOKIE, val, &n) != ESP_OK)
        return false;
    if (strlen(val) != SESS_HEX)
        return false;
    for (int i = 0; i < SESS_N; i++) {
        if (s_sess[i][0] && strcmp(s_sess[i], val) == 0)
            return true;
    }
    return false;
}

void www_session_new(char *cookie, size_t n)
{
    uint8_t raw[SESS_HEX / 2];
    char *tok = s_sess[s_sess_next];
    esp_fill_random(raw, sizeof raw);
    for (size_t i = 0; i < sizeof raw; i++)
        snprintf(tok + 2 * i, 3, "%02x", raw[i]);
    s_sess_next = (uint8_t)((s_sess_next + 1) % SESS_N);
    snprintf(cookie, n, SESS_COOKIE "=%s; Path=/; HttpOnly; SameSite=Strict", tok);
}

bool guest_json(httpd_req_t *req)
{
    if (www_session_ok(req))
        return false;
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, NULL);
    return true;
}

static bool guest_html(httpd_req_t *req)
{
    if (www_session_ok(req))
        return false;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login.html");
    httpd_resp_send(req, NULL, 0);
    return true;
}

static esp_err_t send_blob(httpd_req_t *req, const char *type,
                           const uint8_t *start, const uint8_t *end)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)start, end - start);
}

esp_err_t send_json(httpd_req_t *req, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_send(req, s, strlen(s));
    free(s);
    return e;
}

char *recv_body(httpd_req_t *req)
{
    int n = req->content_len;
    if (n < 0 || n > 4096)
        return NULL;
    char *buf = calloc(1, (size_t)n + 1);
    if (!buf)
        return NULL;
    int got = 0;
    while (got < n) {
        int r = httpd_req_recv(req, buf + got, n - got);
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += r;
    }
    return buf;
}

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login.html");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t login_html_get(httpd_req_t *req)
{
    return send_blob(req, "text/html", login_html_start, login_html_end);
}

static esp_err_t video_html_get(httpd_req_t *req)
{
    return send_blob(req, "text/html", video_html_start, video_html_end);
}

static esp_err_t local_html_get(httpd_req_t *req)
{
    if (guest_html(req))
        return ESP_OK;
    return send_blob(req, "text/html", localNetwork_html_start, localNetwork_html_end);
}

static esp_err_t status_html_get(httpd_req_t *req)
{
    if (guest_html(req))
        return ESP_OK;
    return send_blob(req, "text/html", status_html_start, status_html_end);
}

static esp_err_t capture_html_get(httpd_req_t *req)
{
    if (guest_html(req))
        return ESP_OK;
    return send_blob(req, "text/html", capture_html_start, capture_html_end);
}

static esp_err_t system_html_get(httpd_req_t *req)
{
    if (guest_html(req))
        return ESP_OK;
    return send_blob(req, "text/html", system_html_start, system_html_end);
}

static esp_err_t skystats_html_get(httpd_req_t *req)
{
    if (guest_html(req))
        return ESP_OK;
    return send_blob(req, "text/html", skystats_html_start, skystats_html_end);
}

static esp_err_t overlay_html_get(httpd_req_t *req)
{
    if (guest_html(req))
        return ESP_OK;
    return send_blob(req, "text/html", overlay_html_start, overlay_html_end);
}

static esp_err_t axcss_get(httpd_req_t *req)
{
    return send_blob(req, "text/css", axcss_css_start, axcss_css_end);
}

static esp_err_t js_get(httpd_req_t *req)
{
    return send_blob(req, "application/javascript", axjs_js_start, axjs_js_end);
}

static esp_err_t camera_js_get(httpd_req_t *req)
{
    return send_blob(req, "application/javascript", camera_js_start, camera_js_end);
}

static esp_err_t awb_js_get(httpd_req_t *req)
{
    return send_blob(req, "application/javascript", awb_js_start, awb_js_end);
}

static esp_err_t skymask_js_get(httpd_req_t *req)
{
    return send_blob(req, "application/javascript", skymask_js_start, skymask_js_end);
}

static esp_err_t expauto_js_get(httpd_req_t *req)
{
    return send_blob(req, "application/javascript", expauto_js_start, expauto_js_end);
}

void register_get(httpd_handle_t h, const char *uri, esp_err_t (*fn)(httpd_req_t *))
{
    httpd_uri_t u = { .uri = uri, .method = HTTP_GET, .handler = fn };
    httpd_register_uri_handler(h, &u);
}

void register_post(httpd_handle_t h, const char *uri, esp_err_t (*fn)(httpd_req_t *))
{
    httpd_uri_t u = { .uri = uri, .method = HTTP_POST, .handler = fn };
    httpd_register_uri_handler(h, &u);
}

void www_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HP10_HTTP_PORT;
    cfg.max_uri_handlers = 64;
    cfg.stack_size = 16384;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 60;
    cfg.send_wait_timeout = 60;

    s_http = NULL;
    ESP_ERROR_CHECK(httpd_start(&s_http, &cfg));
    httpd_handle_t http = s_http;

    register_get(http, "/", root_get);
    register_get(http, "/login.html", login_html_get);
    register_get(http, "/video.html", video_html_get);
    register_get(http, "/localNetwork.html", local_html_get);
    register_get(http, "/status.html", status_html_get);
    register_get(http, "/capture.html", capture_html_get);
    register_get(http, "/system.html", system_html_get);
    register_get(http, "/skystats.html", skystats_html_get);
    register_get(http, "/overlay.html", overlay_html_get);
    register_get(http, "/axcss.css", axcss_get);
    register_get(http, "/axjs.js", js_get);
    register_get(http, "/camera.js", camera_js_get);
    register_get(http, "/awb.js", awb_js_get);
    register_get(http, "/skymask.js", skymask_js_get);
    register_get(http, "/expauto.js", expauto_js_get);

    httpd_config_t scfg = HTTPD_DEFAULT_CONFIG();
    scfg.server_port = HP10_STREAM_PORT;
    scfg.ctrl_port = cfg.ctrl_port + 1;
    scfg.max_uri_handlers = 4;
    scfg.stack_size = 8192;
    scfg.lru_purge_enable = true;

    httpd_handle_t stream = NULL;
    ESP_ERROR_CHECK(httpd_start(&stream, &scfg));
    www_api_register(http, stream);
    www_sky_register(http);
    www_overlay_register(http);
    hp10_cam_auto_register(http);

    ESP_LOGI(TAG, "httpd :%d  stream :%d", HP10_HTTP_PORT, HP10_STREAM_PORT);
    xTaskCreate(beat_post_task, "http_beat", 3072, NULL, 3, &s_beat_task);
    xTaskCreate(http_watch_task, "http_wd", 3072, NULL, 3, NULL);
}
