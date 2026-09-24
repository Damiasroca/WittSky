#pragma once

#include "nvs.h"

#include <stdbool.h>
#include <stdint.h>

/* Picture settings from the Camera page. Absent from NVS until the user saves. */

int hp10_cam_cfg_framesize(bool psram);
bool hp10_cam_cfg_saved(void);
void hp10_cam_cfg_note_init(bool psram);
bool hp10_cam_cfg_needs_reboot(void);
const char *hp10_cam_cfg_set(int framesize, int brightness, int contrast,
                             int saturation, int hmirror, int vflip);
void hp10_cam_cfg_get(int *framesize, int *brightness, int *contrast,
                      int *saturation, int *hmirror, int *vflip);
void hp10_cam_cfg_apply(void);
void hp10_cam_cfg_apply_locked(void);
void hp10_cam_cfg_load(nvs_handle_t h);
void hp10_cam_cfg_save(nvs_handle_t h);
