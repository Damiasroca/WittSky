/* Recovered HP10 web UI + the routes the pages actually call.
 * Capture / stream / video JSON are live. OTA is user-URL + stock JSON.
 */

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs.h"

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
extern const uint8_t overlay_html_start[] asm("_binary_overlay_html_start");
extern const uint8_t overlay_html_end[]   asm("_binary_overlay_html_end");
extern const uint8_t axcss_css_start[]     asm("_binary_axcss_css_start");
extern const uint8_t axcss_css_end[]       asm("_binary_axcss_css_end");
extern const uint8_t dark_css_start[]      asm("_binary_dark_css_start");
extern const uint8_t dark_css_end[]        asm("_binary_dark_css_end");
extern const uint8_t warm_css_start[]      asm("_binary_warm_css_start");
extern const uint8_t warm_css_end[]        asm("_binary_warm_css_end");
extern const uint8_t technical_css_start[] asm("_binary_technical_css_start");
extern const uint8_t technical_css_end[]   asm("_binary_technical_css_end");
extern const uint8_t axjs_js_start[] asm("_binary_axjs_js_start");
extern const uint8_t axjs_js_end[]   asm("_binary_axjs_js_end");
extern const uint8_t camera_js_start[] asm("_binary_camera_js_start");
extern const uint8_t camera_js_end[]   asm("_binary_camera_js_end");
extern const uint8_t awb_js_start[] asm("_binary_awb_js_start");
extern const uint8_t awb_js_end[]   asm("_binary_awb_js_end");
extern const uint8_t skymask_js_start[] asm("_binary_skymask_js_start");
extern const uint8_t skymask_js_end[]   asm("_binary_skymask_js_end");


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

/* One row per look. To add one: embed its CSS in CMakeLists.txt and append here.
 * An unknown or missing NVS id falls back to dark. */
static const struct {
    const char *id;
    const char *label;
    const uint8_t *start;
    const uint8_t *end;
} s_themes[] = {
    { "dark",      "Dark",         dark_css_start,      dark_css_end },
    { "warm",      "Station warm", warm_css_start,      warm_css_end },
    { "technical", "Technical",    technical_css_start, technical_css_end },
};

static char s_theme_id[16] = "dark";

static int theme_index(const char *id)
{
    if (!id || !id[0])
        return -1;
    for (size_t i = 0; i < sizeof s_themes / sizeof s_themes[0]; i++) {
        if (strcmp(s_themes[i].id, id) == 0)
            return (int)i;
    }
    return -1;
}

static void theme_load(void)
{
    nvs_handle_t h;
    char buf[sizeof s_theme_id];
    size_t n = sizeof buf;
    if (nvs_open("hp10", NVS_READONLY, &h) != ESP_OK)
        return;
    if (nvs_get_str(h, "theme", buf, &n) == ESP_OK && theme_index(buf) >= 0)
        strlcpy(s_theme_id, buf, sizeof s_theme_id);
    nvs_close(h);
}

static void theme_save(void)
{
    nvs_handle_t h;
    if (nvs_open("hp10", NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_str(h, "theme", s_theme_id);
    nvs_commit(h);
    nvs_close(h);
}

static int theme_active(void)
{
    int i = theme_index(s_theme_id);
    if (i >= 0)
        return i;
    i = theme_index("dark");
    return i >= 0 ? i : 0;
}

static esp_err_t axcss_get(httpd_req_t *req)
{
    return send_blob(req, "text/css", axcss_css_start, axcss_css_end);
}

static esp_err_t active_theme_get(httpd_req_t *req)
{
    int i = theme_active();
    return send_blob(req, "text/css", s_themes[i].start, s_themes[i].end);
}

static esp_err_t get_theme(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "themes");
    cJSON_AddStringToObject(o, "id", s_theme_id);
    for (size_t i = 0; i < sizeof s_themes / sizeof s_themes[0]; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "id", s_themes[i].id);
        cJSON_AddStringToObject(t, "label", s_themes[i].label);
        cJSON_AddItemToArray(arr, t);
    }
    return send_json(req, o);
}

static esp_err_t set_theme(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    char *body = recv_body(req);
    cJSON *in = body ? cJSON_Parse(body) : NULL;
    free(body);
    cJSON *id = in ? cJSON_GetObjectItem(in, "id") : NULL;
    int i = (id && cJSON_IsString(id)) ? theme_index(id->valuestring) : -1;
    cJSON_Delete(in);
    if (i < 0) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "0");
        cJSON_AddStringToObject(o, "msg", "Unknown look");
        return send_json(req, o);
    }
    strlcpy(s_theme_id, s_themes[i].id, sizeof s_theme_id);
    theme_save();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "1");
    cJSON_AddStringToObject(o, "id", s_theme_id);
    return send_json(req, o);
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
    theme_load();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HP10_HTTP_PORT;
    cfg.max_uri_handlers = 64;
    cfg.stack_size = 16384;
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
    register_get(http, "/overlay.html", overlay_html_get);
    register_get(http, "/axcss.css", axcss_get);
    register_get(http, "/themes/active.css", active_theme_get);
    register_get(http, "/get_theme", get_theme);
    register_post(http, "/set_theme", set_theme);
    register_get(http, "/axjs.js", js_get);
    register_get(http, "/camera.js", camera_js_get);
    register_get(http, "/awb.js", awb_js_get);
    register_get(http, "/skymask.js", skymask_js_get);

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

    ESP_LOGI(TAG, "httpd :%d  stream :%d", HP10_HTTP_PORT, HP10_STREAM_PORT);
}
