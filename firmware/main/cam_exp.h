#pragma once

#include "esp_camera.h"
#include "nvs.h"

#include <stdbool.h>
#include <stdint.h>

struct cJSON;

/* Default for clkrc and gain_ceiling: leave the driver's own value alone. */
#define HP10_EXP_SENTINEL 0xFF

typedef struct {
    uint8_t  aec_on;
    uint16_t aec_value;
    uint8_t  aec2;
    int8_t   ae_level;
    uint8_t  agc_on;
    uint8_t  agc_gain;
    uint8_t  gain_ceiling;
    uint8_t  clkrc;
    uint16_t dummy_lines;
} hp10_cam_exp_t;

/* Registers as the sensor reports them right now. Caller holds g_cam_mu. */
typedef struct {
    bool        aec_on;
    bool        agc_on;
    uint16_t    aec_value;
    uint8_t     gain_reg;
    uint8_t     ceil_idx;
    uint16_t    pid;
    framesize_t fs;
} hp10_cam_live_t;

void hp10_cam_exp_init(void);
void hp10_cam_exp_defaults(hp10_cam_exp_t *out);
const char *hp10_cam_exp_validate(const hp10_cam_exp_t *in, hp10_cam_exp_t *out);
bool hp10_cam_exp_saved(void);
const hp10_cam_exp_t *hp10_cam_exp_active(void);
void hp10_cam_exp_get(hp10_cam_exp_t *out);
const char *hp10_cam_exp_update(const hp10_cam_exp_t *in);
const char *hp10_cam_exp_commit(const hp10_cam_exp_t *in);
void hp10_cam_exp_day(const hp10_cam_exp_t *base, hp10_cam_exp_t *out);
void hp10_cam_exp_patch_json(const struct cJSON *o, hp10_cam_exp_t *draft);
void hp10_cam_exp_add_json(struct cJSON *o);
void hp10_cam_exp_load(nvs_handle_t h);
void hp10_cam_exp_save(nvs_handle_t h);
int hp10_cam_exp_apply(const hp10_cam_exp_t *cfg);
void hp10_cam_exp_apply_locked(const hp10_cam_exp_t *cfg);
void hp10_cam_exp_apply_active_locked(void);
void hp10_cam_exp_post_apply(const hp10_cam_exp_t *applied);
const char *hp10_cam_exp_last_error(void);

bool hp10_cam_exp_is_ov2640(void);
uint16_t hp10_cam_exp_base_lines(framesize_t fs);
uint32_t hp10_cam_exp_estimated_ms(void);
uint32_t hp10_cam_exp_measured_ms(void);

/* The three below need g_cam_mu held. */
void hp10_cam_exp_capture_defaults(void);
void hp10_cam_exp_forget_defaults(void);
bool hp10_cam_exp_read_live(hp10_cam_live_t *out);

float hp10_cam_gain_multiplier(uint8_t gain_reg);
