#include "overlay_eco.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "overlay_eco_parse.h"

#include <stdio.h>
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

esp_err_t ov_eco_fetch_url(ov_sample_t *out, const char *url,
                           int timeout_ms, const char **why)
{
    char *body = NULL;
    const char *parse_why = NULL;
    esp_http_client_handle_t cli = NULL;
    esp_err_t err = ESP_FAIL;
    int total = 0;
    int https;
    const int cap = 16384;

    if (why)
        *why = "weather fetch failed";
    if (!out)
        return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    if (!url || !url[0]) {
        if (why)
            *why = "station address";
        return ESP_ERR_INVALID_ARG;
    }
    https = strncmp(url, "https://", 8) == 0;

    body = psram(cap);
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
    if (esp_http_client_open(cli, 0) != ESP_OK) {
        int tls_code = 0;
        int tls_flags = 0;
        esp_http_client_get_and_clear_last_tls_error(cli, &tls_code, &tls_flags);
        if (tls_code) {
            ESP_LOGW(TAG, "ecowitt tls mbedtls=%d flags=0x%x", tls_code, tls_flags);
            if (why)
                *why = "tls handshake";
        } else if (why) {
            *why = "station unreachable";
        }
        goto done;
    }
    esp_http_client_fetch_headers(cli);
    if (esp_http_client_get_status_code(cli) != 200) {
        if (why)
            *why = "station http status";
        goto done;
    }
    while (total < cap - 1) {
        int r = esp_http_client_read(cli, body + total, cap - 1 - total);
        if (r < 0) {
            if (why)
                *why = "station read";
            goto done;
        }
        if (r == 0)
            break;
        total += r;
    }
    if (total >= cap - 1) {
        if (why)
            *why = "station response too large";
        goto done;
    }
    body[total] = 0;
    if (ov_eco_parse(body, out, &parse_why) != 0) {
        if (why)
            *why = parse_why ? parse_why : "malformed";
        ESP_LOGW(TAG, "ecowitt body: %.120s", body);
        goto done;
    }
    err = ESP_OK;
    if (why)
        *why = NULL;
    ESP_LOGI(TAG, "ecowitt wind %d mm/s dir %d temp %d mC rain %d",
             (int)out->wind_mms, out->dir_deg, (int)out->temp_mc,
             out->raining ? 1 : 0);

done:
    if (cli) {
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
    }
    free(body);
    return err;
}

esp_err_t ov_eco_fetch(ov_sample_t *out, const char *host, int port,
                       int timeout_ms, const char **why)
{
    char url[160];

    if (!host || !host[0] || port < 1 || port > 65535) {
        if (why)
            *why = "station address";
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(url, sizeof url, "http://%s:%d/get_livedata_info", host, port) >= (int)sizeof url) {
        if (why)
            *why = "station address";
        return ESP_ERR_INVALID_ARG;
    }
    return ov_eco_fetch_url(out, url, timeout_ms, why);
}
