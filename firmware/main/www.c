/* Recovered HP10 web UI + the routes the pages actually call.
 * Capture / stream / video JSON are live. OTA is user-URL + stock JSON.
 */

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

#include "hp10_bringup.h"
#include "pins.h"
#include "www.h"
#include "www_priv.h"

static const char *TAG = "www";

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
extern const uint8_t axcss_css_start[] asm("_binary_axcss_css_start");
extern const uint8_t axcss_css_end[]   asm("_binary_axcss_css_end");
extern const uint8_t axjs_js_start[] asm("_binary_axjs_js_start");
extern const uint8_t axjs_js_end[]   asm("_binary_axjs_js_end");


bool guest_json(httpd_req_t *req)
{
    if (g_bLoggedIn)
        return false;
    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, NULL);
    return true;
}

static bool guest_html(httpd_req_t *req)
{
    if (g_bLoggedIn)
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

static esp_err_t css_get(httpd_req_t *req)
{
    return send_blob(req, "text/css", axcss_css_start, axcss_css_end);
}

static esp_err_t js_get(httpd_req_t *req)
{
    return send_blob(req, "application/javascript", axjs_js_start, axjs_js_end);
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
    cfg.max_uri_handlers = 40;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 60;
    cfg.send_wait_timeout = 60;

    httpd_handle_t http = NULL;
    ESP_ERROR_CHECK(httpd_start(&http, &cfg));

    register_get(http, "/", root_get);
    register_get(http, "/login.html", login_html_get);
    register_get(http, "/video.html", video_html_get);
    register_get(http, "/localNetwork.html", local_html_get);
    register_get(http, "/status.html", status_html_get);
    register_get(http, "/capture.html", capture_html_get);
    register_get(http, "/system.html", system_html_get);
    register_get(http, "/skystats.html", skystats_html_get);
    register_get(http, "/axcss.css", css_get);
    register_get(http, "/axjs.js", js_get);

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

    ESP_LOGI(TAG, "httpd :%d  stream :%d", HP10_HTTP_PORT, HP10_STREAM_PORT);
}
