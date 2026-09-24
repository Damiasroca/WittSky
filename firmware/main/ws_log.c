/* Isolated websocket logger.
 *
 * Does not share the camera mutex, HTTP (80), or MJPEG (81) servers.
 * When enabled it listens on HP10_WS_LOG_PORT for local viewers. A ws://
 * or wss:// URL in g_ws_log_url also pushes the same stream as a client.
 */

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_websocket_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "hp10_bringup.h"
#include "pins.h"

#if !CONFIG_HTTPD_WS_SUPPORT
#error "CONFIG_HTTPD_WS_SUPPORT must be enabled for websocket logs"
#endif

static const char *TAG = "ws_log";

bool     g_ws_log_en;
char     g_ws_log_url[HP10_UPLOAD_URL_MAX];
uint8_t  g_ws_log_level = ESP_LOG_INFO;

#define RING_SZ      4096
#define LOG_LINE_MAX 192
#define MAX_CLIENTS  3
#define SKIP_TAGS    "ws_log", "websocket_client", "httpd_ws", "httpd"

static char                  s_ring[RING_SZ];
static size_t                s_rhead, s_rtail;
static SemaphoreHandle_t     s_ring_mu;
static TaskHandle_t          s_task;
static volatile uint32_t     s_cfg_gen;
static volatile bool         s_hooked;

static httpd_handle_t                    s_httpd;
static char                              s_uri[64] = HP10_WS_LOG_PATH;
static esp_websocket_client_handle_t     s_cli;
static volatile bool                     s_cli_ok;
static char                              s_status[96] = "off";
static volatile bool                     s_run;
static volatile uint8_t                  s_level = ESP_LOG_INFO;

static bool url_is_remote(const char *u)
{
    return u && (!strncmp(u, "ws://", 5) || !strncmp(u, "wss://", 6));
}

static bool path_ok(const char *u)
{
    if (!u || u[0] != '/' || u[1] == '/')
        return false;
    size_t n = 0;
    for (const char *p = u; *p; p++, n++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '/' || c == '_' ||
                  c == '-' || c == '.';
        if (!ok || n >= 62)
            return false;
    }
    return n > 0;
}

bool hp10_ws_log_url_ok(const char *url)
{
    if (!url || !url[0])
        return true;
    if (strlen(url) >= HP10_UPLOAD_URL_MAX)
        return false;
    if (url_is_remote(url)) {
        const char *h = url + (url[2] == 's' ? 6 : 5);
        return *h && *h != '/' && !strchr(url, ' ');
    }
    return path_ok(url);
}

static const char *listen_path(const char *url)
{
    if (path_ok(url))
        return url;
    return HP10_WS_LOG_PATH;
}

static int line_level(const char *s)
{
    while (s && *s) {
        if (*s == '\033') {
            while (*s && *s != 'm')
                s++;
            if (*s == 'm')
                s++;
            continue;
        }
        if (*s == 'E' || *s == 'W' || *s == 'I' || *s == 'D' || *s == 'V') {
            switch (*s) {
            case 'E': return ESP_LOG_ERROR;
            case 'W': return ESP_LOG_WARN;
            case 'I': return ESP_LOG_INFO;
            case 'D': return ESP_LOG_DEBUG;
            case 'V': return ESP_LOG_VERBOSE;
            }
        }
        break;
    }
    return ESP_LOG_INFO;
}

static bool skip_send(const char *s)
{
    static const char *tags[] = { SKIP_TAGS };
    for (size_t i = 0; i < sizeof tags / sizeof tags[0]; i++) {
        char needle[24];
        snprintf(needle, sizeof needle, " %s:", tags[i]);
        if (strstr(s, needle))
            return true;
    }
    return false;
}

static void ring_put(const char *s, int n)
{
    if (!s_ring_mu || n <= 0)
        return;
    if (xSemaphoreTake(s_ring_mu, 0) != pdTRUE)
        return;
    for (int i = 0; i < n; i++) {
        size_t next = (s_rtail + 1) % RING_SZ;
        if (next == s_rhead)
            s_rhead = (s_rhead + 1) % RING_SZ;
        s_ring[s_rtail] = s[i];
        s_rtail = next;
    }
    xSemaphoreGive(s_ring_mu);
}

static int ring_pop(char *out, int cap)
{
    if (!s_ring_mu || cap < 2)
        return 0;
    if (xSemaphoreTake(s_ring_mu, 0) != pdTRUE)
        return 0;
    int n = 0;
    while (s_rhead != s_rtail && n < cap - 1) {
        char c = s_ring[s_rhead];
        s_rhead = (s_rhead + 1) % RING_SZ;
        out[n++] = c;
        if (c == '\n')
            break;
    }
    xSemaphoreGive(s_ring_mu);
    out[n] = 0;
    return n;
}

static int ws_vprintf(const char *fmt, va_list ap)
{
    if (s_run) {
        char line[LOG_LINE_MAX];
        va_list copy;
        va_copy(copy, ap);
        int n = vsnprintf(line, sizeof line, fmt, copy);
        va_end(copy);
        if (n > 0) {
            if (n >= (int)sizeof line) {
                line[sizeof line - 2] = '\n';
                n = (int)sizeof line - 1;
                line[n] = 0;
            }
            if ((uint8_t)line_level(line) <= s_level && !skip_send(line)) {
                ring_put(line, n);
                if (s_task)
                    xTaskNotifyGive(s_task);
            }
        }
    }
    return vprintf(fmt, ap);
}

static void set_status(const char *s)
{
    strlcpy(s_status, s, sizeof s_status);
}

void hp10_ws_log_status(char *buf, size_t n)
{
    if (!buf || !n)
        return;
    strlcpy(buf, s_status, n);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET)
        return ESP_OK;

    httpd_ws_frame_t f = { 0 };
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK)
        return ESP_FAIL;
    if (f.len == 0)
        return ESP_OK;
    uint8_t *buf = calloc(1, f.len + 1);
    if (!buf)
        return ESP_ERR_NO_MEM;
    f.payload = buf;
    esp_err_t e = httpd_ws_recv_frame(req, &f, f.len);
    free(buf);
    return e;
}

static void listen_stop(void)
{
    if (!s_httpd)
        return;
    httpd_stop(s_httpd);
    s_httpd = NULL;
}

static void listen_start(const char *path)
{
    listen_stop();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HP10_WS_LOG_PORT;
    cfg.ctrl_port = HP10_WS_LOG_CTRL_PORT;
    cfg.max_open_sockets = MAX_CLIENTS;
    cfg.max_uri_handlers = 2;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 4096;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        s_httpd = NULL;
        ESP_LOGW(TAG, "listen httpd_start failed");
        return;
    }
    strlcpy(s_uri, path, sizeof s_uri);
    httpd_uri_t u = {
        .uri = s_uri,
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };
    if (httpd_register_uri_handler(s_httpd, &u) != ESP_OK) {
        ESP_LOGW(TAG, "listen register '%s' failed", s_uri);
        listen_stop();
    }
}

static void cli_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == WEBSOCKET_EVENT_CONNECTED)
        s_cli_ok = true;
    else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR)
        s_cli_ok = false;
}

static void client_stop(void)
{
    if (!s_cli)
        return;
    esp_websocket_client_close(s_cli, pdMS_TO_TICKS(200));
    esp_websocket_client_stop(s_cli);
    esp_websocket_client_destroy(s_cli);
    s_cli = NULL;
    s_cli_ok = false;
}

static void client_start(const char *url)
{
    client_stop();
    bool wss = strncmp(url, "wss://", 6) == 0;
    esp_websocket_client_config_t cfg = {
        .uri = url,
        .buffer_size = 1024,
        .task_stack = 4096,
        .task_prio = 2,
        .reconnect_timeout_ms = 4000,
        .network_timeout_ms = 8000,
        .crt_bundle_attach = wss ? esp_crt_bundle_attach : NULL,
    };
    s_cli = esp_websocket_client_init(&cfg);
    if (!s_cli) {
        ESP_LOGW(TAG, "client init failed");
        return;
    }
    esp_websocket_register_events(s_cli, WEBSOCKET_EVENT_ANY, cli_evt, NULL);
    if (esp_websocket_client_start(s_cli) != ESP_OK) {
        ESP_LOGW(TAG, "client start failed");
        client_stop();
    }
}

static void refresh_status(const char *path, bool remote, const char *url)
{
    if (!s_run) {
        set_status("off");
        return;
    }
    char buf[96];
    if (remote && s_cli_ok)
        snprintf(buf, sizeof buf, "listen :%u%s + push ok",
                 HP10_WS_LOG_PORT, path);
    else if (remote && s_cli)
        snprintf(buf, sizeof buf, "listen :%u%s + pushing %s",
                 HP10_WS_LOG_PORT, path, url);
    else
        snprintf(buf, sizeof buf, "listen :%u%s", HP10_WS_LOG_PORT, path);
    set_status(buf);
}

static void transports_rebuild(void)
{
    bool en = g_ws_log_en;
    char url[HP10_UPLOAD_URL_MAX];
    uint8_t level = g_ws_log_level;
    strlcpy(url, g_ws_log_url, sizeof url);
    if (level < ESP_LOG_ERROR)
        level = ESP_LOG_ERROR;
    if (level > ESP_LOG_VERBOSE)
        level = ESP_LOG_VERBOSE;
    if (!hp10_ws_log_url_ok(url))
        url[0] = 0;

    s_run = false;
    listen_stop();
    client_stop();

    if (!en) {
        s_level = ESP_LOG_INFO;
        esp_log_level_set("*", ESP_LOG_INFO);
        set_status("off");
        return;
    }

    s_level = level;
    esp_log_level_set("*", (esp_log_level_t)level);

    const char *path = listen_path(url);
    listen_start(path);
    bool remote = url_is_remote(url);
    if (remote)
        client_start(url);

    s_run = true;
    refresh_status(path, remote, url);
    ESP_LOGI(TAG, "armed path='%s' remote=%d url='%s' level=%u",
             path, remote ? 1 : 0, url, (unsigned)level);
}

static void broadcast(const char *line, int n)
{
    if (!s_httpd || n <= 0)
        return;
    httpd_ws_frame_t pkt = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)line,
        .len = (size_t)n,
    };
    size_t nfds = MAX_CLIENTS;
    int fds[MAX_CLIENTS];
    if (httpd_get_client_list(s_httpd, &nfds, fds) != ESP_OK)
        return;
    for (size_t i = 0; i < nfds; i++) {
        if (httpd_ws_get_fd_info(s_httpd, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET)
            continue;
        httpd_ws_send_data(s_httpd, fds[i], &pkt);
    }
}

static void push_remote(const char *line, int n)
{
    if (!s_cli || !s_cli_ok || n <= 0)
        return;
    if (!esp_websocket_client_is_connected(s_cli)) {
        s_cli_ok = false;
        return;
    }
    esp_websocket_client_send_text(s_cli, line, n, pdMS_TO_TICKS(50));
}

static void log_task(void *arg)
{
    (void)arg;
    uint32_t gen = 0;
    TickType_t heal_at = 0;
    transports_rebuild();
    gen = s_cfg_gen;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
        if (gen != s_cfg_gen) {
            gen = s_cfg_gen;
            transports_rebuild();
            heal_at = xTaskGetTickCount();
        }
        if (s_run && g_ws_log_en &&
            (xTaskGetTickCount() - heal_at) > pdMS_TO_TICKS(10000)) {
            bool acted = false;
            if (!s_httpd) {
                listen_start(listen_path(g_ws_log_url));
                acted = true;
            }
            if (!s_cli && url_is_remote(g_ws_log_url) && hp10_sta_has_ip()) {
                client_start(g_ws_log_url);
                acted = true;
            }
            if (acted)
                heal_at = xTaskGetTickCount();
        }
        if (url_is_remote(g_ws_log_url))
            refresh_status(listen_path(g_ws_log_url), true, g_ws_log_url);

        char line[LOG_LINE_MAX];
        int n;
        int burst = 0;
        while (burst++ < 32 && (n = ring_pop(line, sizeof line)) > 0) {
            broadcast(line, n);
            push_remote(line, n);
        }
    }
}

void hp10_ws_log_apply(void)
{
    s_cfg_gen++;
    if (s_task)
        xTaskNotifyGive(s_task);
}

void hp10_ws_log_start(void)
{
    if (s_task)
        return;
    s_ring_mu = xSemaphoreCreateMutex();
    if (!s_hooked) {
        esp_log_set_vprintf(ws_vprintf);
        s_hooked = true;
    }
    xTaskCreate(log_task, "ws_log", 4096, NULL, 2, &s_task);
}
