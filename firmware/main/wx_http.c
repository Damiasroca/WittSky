#include "wx_http.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "overlay";

static void *psram(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p)
        p = malloc(n);
    return p;
}

esp_err_t wx_fetch_url(const char *url, const char *hdr, const char *hdr_val,
                       int timeout_ms,
                       int (*parse)(const char *json, ov_sample_t *out, const char **why),
                       ov_sample_t *out, const char **why, const char *who)
{
    char *body = NULL;
    const char *parse_why = NULL;
    esp_http_client_handle_t cli = NULL;
    esp_err_t err = ESP_FAIL;
    int total = 0;
    int https;

    if (why)
        *why = "weather fetch failed";
    if (!out || !parse || !url || !url[0]) {
        if (why)
            *why = "station address";
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof *out);
    https = strncmp(url, "https://", 8) == 0;
    body = psram(WX_BODY_CAP);
    if (!body) {
        if (why)
            *why = "out of memory";
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = timeout_ms > 0 ? timeout_ms : 3000,
        .crt_bundle_attach = https ? esp_crt_bundle_attach : NULL,
    };
    cli = esp_http_client_init(&cfg);
    if (!cli) {
        free(body);
        if (why)
            *why = "http client";
        return ESP_FAIL;
    }
    if (hdr && hdr[0] && hdr_val)
        esp_http_client_set_header(cli, hdr, hdr_val);
    if (esp_http_client_open(cli, 0) != ESP_OK) {
        int tls_code = 0;
        int tls_flags = 0;
        esp_http_client_get_and_clear_last_tls_error(cli, &tls_code, &tls_flags);
        if (tls_code) {
            ESP_LOGW(TAG, "%s tls mbedtls=%d flags=0x%x", who ? who : "wx",
                     tls_code, tls_flags);
            if (why)
                *why = "tls handshake";
        } else if (why) {
            *why = "station unreachable";
        }
        goto done;
    }
    esp_http_client_fetch_headers(cli);
    {
        int sc = esp_http_client_get_status_code(cli);
        if (sc != 200) {
            ESP_LOGW(TAG, "%s http status %d", who ? who : "wx", sc);
            if (why)
                *why = "station http status";
            goto done;
        }
    }
    while (total < WX_BODY_CAP - 1) {
        int r = esp_http_client_read(cli, body + total, WX_BODY_CAP - 1 - total);
        if (r < 0) {
            if (why)
                *why = "station read";
            goto done;
        }
        if (r == 0)
            break;
        total += r;
    }
    if (!esp_http_client_is_complete_data_received(cli)) {
        if (why)
            *why = "station response too large";
        goto done;
    }
    body[total] = 0;
    if (parse(body, out, &parse_why) != 0) {
        if (why)
            *why = parse_why ? parse_why : "malformed";
        ESP_LOGW(TAG, "%s body: %.120s", who ? who : "wx", body);
        goto done;
    }
    err = ESP_OK;
    if (why)
        *why = NULL;

done:
    if (cli) {
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
    }
    free(body);
    return err;
}
