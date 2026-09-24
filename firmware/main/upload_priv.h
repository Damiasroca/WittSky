#pragma once

#include "esp_camera.h"
#include "esp_err.h"
#include "esp_http_client.h"

#include <stddef.h>
#include <stdbool.h>

typedef struct {
    char buf[768];
    int  n;
} http_acc_t;

bool want_custom(void);
bool want_ecowitt(void);
const char *dest_name(void);
void log_sta(void);
void log_cfg(const char *when);

camera_fb_t *grab_frame(void);
void drop_frame(camera_fb_t *fb);
void sky_stats_on_frame(const camera_fb_t *fb, char *json, size_t json_n);

esp_err_t http_evt(esp_http_client_event_t *evt);
void log_http_result(const char *kind, const char *url,
                     esp_http_client_handle_t cli, esp_err_t err,
                     http_acc_t *acc);
void *psram_malloc(size_t n);

esp_err_t upload_ecowitt(void);
