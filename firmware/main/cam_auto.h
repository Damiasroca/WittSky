#pragma once

#include "esp_http_server.h"

#include <stdbool.h>
#include <stdint.h>

struct cJSON;

typedef enum {
    HP10_MODE_UNKNOWN = 0,
    HP10_MODE_DAY = 1,
    HP10_MODE_NIGHT = 2,
} hp10_cam_mode_t;

/* Loads the saved switching settings and night preset, and starts the
 * sampling task. Call once, after the camera mutex exists. */
void hp10_cam_auto_start(void);

const char *hp10_cam_auto_save_night(void);
const char *hp10_cam_auto_apply(hp10_cam_mode_t mode, bool automatic, float trigger_ev);
void hp10_cam_auto_add_json(struct cJSON *o);
void hp10_cam_auto_register(httpd_handle_t http);
