/* Night / day exposure switching, ported from the CAMERA_KERRY project.
 *
 * A background task decodes a downscaled frame now and then, turns the mean
 * brightness plus the sensor's live exposure and gain into a scene EV, and
 * swaps between the saved night exposure and its day twin when that EV stays
 * past a threshold for several samples in a row.
 *
 * Nothing here waits forever on the camera. Port 80 has one worker and the
 * stream, capture and upload paths share g_cam_mu, so every wait is bounded.
 */

#include "cam_auto.h"

#include "cJSON.h"
#include "sdkconfig.h"

#if !CONFIG_JD_USE_ROM
#error luma decode uses the ROM tjpgd row callback (CONFIG_JD_USE_ROM)
#endif
#include "rom/tjpgd.h"

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cam_exp.h"
#include "hp10_bringup.h"
#include "www_priv.h"

static const char *TAG = "autosw";

#define NVS_NS            "hp10"
#define KEY_NIGHT         "exp_night"
#define KEY_ASW_CFG       "asw_cfg"
#define KEY_ASW_MODE      "asw_mode"
#define TICK_MS           5000
#define TELEMETRY_MS      60000UL
#define CAM_LOCK_MS       3000
#define POLL_WINDOW_MS    120000UL
/* Internal RAM is tight (about 12 KB free at runtime), and a task stack comes
 * out of it. 6 KB measured 3.8 KB of headroom. Do not grow it casually. */
#define TASK_STACK        6144

typedef struct {
    bool     enabled;
    int16_t  low_ev_tenths;
    int16_t  high_ev_tenths;
    uint8_t  interval_min;
    uint8_t  samples;
    uint8_t  min_dwell_min;
} asw_cfg_t;

typedef struct {
    uint8_t  version;
    uint8_t  enabled;
    int16_t  low_ev_tenths;
    int16_t  high_ev_tenths;
    uint8_t  interval_min;
    uint8_t  samples;
    uint8_t  min_dwell_min;
} asw_blob_t;

typedef struct {
    uint8_t  version;
    uint8_t  aec_on;
    uint16_t aec_value;
    uint8_t  aec2;
    int8_t   ae_level;
    uint8_t  agc_on;
    uint8_t  agc_gain;
    uint8_t  gain_ceiling;
    uint8_t  clkrc;
    uint16_t dummy_lines;
} night_blob_t;

typedef struct {
    uint8_t  avg;
    uint8_t  min;
    uint8_t  max;
    uint16_t lin_mean;
} luma_t;

typedef enum {
    SCENE_OK = 0,
    SCENE_CLIPPED_HIGH,
    SCENE_CLIPPED_LOW,
    SCENE_UNAVAILABLE,
} scene_status_t;

typedef struct {
    scene_status_t status;
    float          ev;
    float          exposure_ms;
    float          gain;
} scene_t;

typedef struct {
    const char     *message;
    hp10_cam_mode_t mode;
    bool            ok;
    bool            automatic;
    float           ev;
    uint32_t        ms;
    bool            valid;
} switch_event_t;

static SemaphoreHandle_t s_mu;
static asw_cfg_t         s_cfg = { false, -60, -40, 2, 3, 15 };
static hp10_cam_mode_t   s_mode;
static uint8_t           s_low_count;
static uint8_t           s_high_count;
static luma_t            s_last_luma;
static bool              s_have_last;
static uint32_t          s_last_sample_ms;
static uint32_t          s_next_sample_ms;
static scene_t           s_last_scene;
static bool              s_have_scene;
static switch_event_t    s_last_switch;
static uint32_t          s_last_switch_ms;
static bool              s_have_last_switch;
static hp10_cam_exp_t    s_night;
static bool              s_has_night;
static bool              s_applying;
static uint32_t          s_next_telemetry_ms;
static TaskHandle_t      s_task;
static volatile hp10_cam_mode_t s_pending;
static volatile bool     s_settle_req;
static volatile bool     s_polled;
static volatile uint32_t s_last_poll_ms;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lock(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mu);
}

static const char *mode_name(hp10_cam_mode_t m)
{
    if (m == HP10_MODE_DAY)
        return "day";
    if (m == HP10_MODE_NIGHT)
        return "night";
    return "unknown";
}

static const char *scene_status_name(scene_status_t s)
{
    switch (s) {
    case SCENE_OK:
        return "ok";
    case SCENE_CLIPPED_HIGH:
        return "clipped_high";
    case SCENE_CLIPPED_LOW:
        return "clipped_low";
    default:
        return "unavailable";
    }
}

/* ---- persistence -------------------------------------------------------- */

static const char *validate_cfg(const asw_cfg_t *c)
{
    if (!c)
        return "Missing settings";
    if (c->low_ev_tenths < -200 || c->low_ev_tenths > 200 ||
        c->high_ev_tenths < -200 || c->high_ev_tenths > 200 ||
        (int)c->low_ev_tenths + 10 > (int)c->high_ev_tenths)
        return "EV thresholds out of range";
    if (c->interval_min < 1 || c->interval_min > 60)
        return "intervalMin out of range";
    if (c->samples < 1 || c->samples > 10)
        return "samples out of range";
    if (c->min_dwell_min > 240)
        return "minDwellMin out of range";
    return NULL;
}

static void save_cfg(const asw_cfg_t *c)
{
    nvs_handle_t h;
    asw_blob_t b = { 0 };
    b.version = 2;
    b.enabled = c->enabled ? 1 : 0;
    b.low_ev_tenths = c->low_ev_tenths;
    b.high_ev_tenths = c->high_ev_tenths;
    b.interval_min = c->interval_min;
    b.samples = c->samples;
    b.min_dwell_min = c->min_dwell_min;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_blob(h, KEY_ASW_CFG, &b, sizeof b);
    nvs_commit(h);
    nvs_close(h);
}

static void save_mode(hp10_cam_mode_t m)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u8(h, KEY_ASW_MODE, (uint8_t)m);
    nvs_commit(h);
    nvs_close(h);
}

static void save_night(const hp10_cam_exp_t *c)
{
    nvs_handle_t h;
    night_blob_t b = { 0 };
    b.version = 1;
    b.aec_on = c->aec_on;
    b.aec_value = c->aec_value;
    b.aec2 = c->aec2;
    b.ae_level = c->ae_level;
    b.agc_on = c->agc_on;
    b.agc_gain = c->agc_gain;
    b.gain_ceiling = c->gain_ceiling;
    b.clkrc = c->clkrc;
    b.dummy_lines = c->dummy_lines;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_blob(h, KEY_NIGHT, &b, sizeof b);
    nvs_commit(h);
    nvs_close(h);
}

static void load_all(void)
{
    nvs_handle_t h;
    asw_cfg_t cfg = { false, -60, -40, 2, 3, 15 };
    hp10_cam_mode_t mode = HP10_MODE_UNKNOWN;
    hp10_cam_exp_t night;
    bool has_night = false;
    bool reset = false;

    hp10_cam_exp_defaults(&night);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        asw_blob_t ab;
        size_t n = sizeof ab;
        uint8_t m = 0;
        night_blob_t nb;
        size_t nn = sizeof nb;

        if (nvs_get_blob(h, KEY_ASW_CFG, &ab, &n) == ESP_OK) {
            if (n == sizeof ab && ab.version == 2) {
                cfg.enabled = ab.enabled != 0;
                cfg.low_ev_tenths = ab.low_ev_tenths;
                cfg.high_ev_tenths = ab.high_ev_tenths;
                cfg.interval_min = ab.interval_min;
                cfg.samples = ab.samples;
                cfg.min_dwell_min = ab.min_dwell_min;
                if (validate_cfg(&cfg))
                    reset = true;
            } else {
                reset = true;
            }
        }
        if (nvs_get_u8(h, KEY_ASW_MODE, &m) == ESP_OK &&
            (m == HP10_MODE_DAY || m == HP10_MODE_NIGHT))
            mode = (hp10_cam_mode_t)m;
        if (nvs_get_blob(h, KEY_NIGHT, &nb, &nn) == ESP_OK &&
            nn == sizeof nb && nb.version == 1) {
            hp10_cam_exp_t t;
            t.aec_on = nb.aec_on;
            t.aec_value = nb.aec_value;
            t.aec2 = nb.aec2;
            t.ae_level = nb.ae_level;
            t.agc_on = nb.agc_on;
            t.agc_gain = nb.agc_gain;
            t.gain_ceiling = nb.gain_ceiling;
            t.clkrc = nb.clkrc;
            t.dummy_lines = nb.dummy_lines;
            /* Checked against the largest frame: the camera is not up yet. */
            if (hp10_cam_exp_validate(&t, &night) == NULL)
                has_night = true;
            else
                ESP_LOGW(TAG, "saved night preset is not valid, ignored");
        }
        nvs_close(h);
    }
    if (reset) {
        cfg = (asw_cfg_t){ false, -60, -40, 2, 3, 15 };
        save_cfg(&cfg);
        ESP_LOGW(TAG, "switching settings were reset, stored values were not usable");
    }
    lock();
    s_cfg = cfg;
    s_mode = mode;
    s_low_count = 0;
    s_high_count = 0;
    s_have_last = false;
    s_have_scene = false;
    s_have_last_switch = false;
    s_next_sample_ms = 0;
    memset(&s_last_switch, 0, sizeof s_last_switch);
    s_night = night;
    s_has_night = has_night;
    unlock();
}

/* ---- sampling ----------------------------------------------------------- */

typedef struct {
    const uint8_t *jpeg;
    size_t         len;
    size_t         pos;
    uint64_t       sum;
    uint64_t       lin_sum;
    uint32_t       count;
    uint8_t        min_y;
    uint8_t        max_y;
} luma_dec_t;

static uint16_t s_lut[256];
static volatile bool s_lut_ready;

static void ensure_lut(void)
{
    if (s_lut_ready)
        return;
    for (int i = 0; i < 256; i++) {
        long r = lroundf(65535.0f * powf((float)i / 255.0f, 2.2f));
        s_lut[i] = (uint16_t)(r < 0 ? 0 : (r > 65535 ? 65535 : r));
    }
    s_lut_ready = true;
}

static UINT luma_in(JDEC *jd, BYTE *buf, UINT n)
{
    luma_dec_t *d = jd->device;
    if (d->pos >= d->len || n == 0)
        return 0;
    if (n > d->len - d->pos)
        n = (UINT)(d->len - d->pos);
    if (buf)
        memcpy(buf, d->jpeg + d->pos, n);
    d->pos += n;
    return n;
}

static UINT luma_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    luma_dec_t *d = jd->device;
    const uint8_t *src = bitmap;
    int rw, rh;
    if (!bitmap || !rect || rect->left > rect->right || rect->top > rect->bottom)
        return 0;
    rw = rect->right - rect->left + 1;
    rh = rect->bottom - rect->top + 1;
    for (int row = 0; row < rh; row++) {
        const uint8_t *px = src + (size_t)row * (size_t)rw * 3;
        for (int col = 0; col < rw; col++) {
            uint8_t y = (uint8_t)((77u * px[0] + 150u * px[1] + 29u * px[2]) >> 8);
            d->sum += y;
            d->lin_sum += s_lut[y];
            if (++d->count == 1) {
                d->min_y = y;
                d->max_y = y;
            } else {
                if (y < d->min_y)
                    d->min_y = y;
                if (y > d->max_y)
                    d->max_y = y;
            }
            px += 3;
        }
    }
    return 1;
}

static bool luma_from_jpeg(const uint8_t *jpeg, size_t len, luma_t *out)
{
    luma_dec_t d;
    JDEC jd;
    uint8_t *work;
    JRESULT jr;
    BYTE scale = 2;

    if (!jpeg || len < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8)
        return false;
    ensure_lut();
    memset(&d, 0, sizeof d);
    d.jpeg = jpeg;
    d.len = len;
    work = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!work)
        work = malloc(4096);
    if (!work)
        return false;
    jr = jd_prepare(&jd, luma_in, work, 4096, &d);
    if (jr != JDR_OK || jd.width < 8 || jd.height < 8) {
        free(work);
        return false;
    }
    if (jd.width > 1280 || jd.height > 960)
        scale = 3;
    jr = jd_decomp(&jd, luma_out, scale);
    free(work);
    if (jr != JDR_OK || d.count == 0)
        return false;
    out->avg = (uint8_t)(d.sum / d.count);
    out->min = d.min_y;
    out->max = d.max_y;
    out->lin_mean = (uint16_t)(d.lin_sum / d.count);
    return true;
}

/* A copy, so the camera goes back to the stream and uploads before the
 * decode starts. One frame is dropped first: with a single buffer the first
 * one out can be the one queued before the last exposure change. */
static bool grab_jpeg_copy(uint8_t **jpeg, size_t *len)
{
    camera_fb_t *fb;
    uint8_t *copy;

    *jpeg = NULL;
    *len = 0;
    if (!g_cam_mu || !g_bCameraOk)
        return false;
    if (xSemaphoreTake(g_cam_mu, pdMS_TO_TICKS(CAM_LOCK_MS)) != pdTRUE)
        return false;
    fb = esp_camera_fb_get();
    if (fb)
        esp_camera_fb_return(fb);
    fb = esp_camera_fb_get();
    if (!fb || fb->format != PIXFORMAT_JPEG || fb->len == 0) {
        if (fb)
            esp_camera_fb_return(fb);
        xSemaphoreGive(g_cam_mu);
        return false;
    }
    copy = heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy)
        copy = malloc(fb->len);
    if (copy) {
        memcpy(copy, fb->buf, fb->len);
        *len = fb->len;
    }
    esp_camera_fb_return(fb);
    xSemaphoreGive(g_cam_mu);
    *jpeg = copy;
    return copy != NULL;
}

static bool sample_luma(luma_t *out)
{
    uint8_t *jpeg;
    size_t len;
    bool ok = grab_jpeg_copy(&jpeg, &len);
    if (ok)
        ok = luma_from_jpeg(jpeg, len, out);
    free(jpeg);
    return ok;
}

static bool read_live(hp10_cam_live_t *live)
{
    bool ok = false;
    if (!g_cam_mu || !g_bCameraOk)
        return false;
    if (xSemaphoreTake(g_cam_mu, pdMS_TO_TICKS(1000)) != pdTRUE)
        return false;
    if (g_bCameraOk)
        ok = hp10_cam_exp_read_live(live);
    xSemaphoreGive(g_cam_mu);
    return ok;
}

static void scene_compute(const luma_t *luma, const hp10_cam_live_t *live,
                          const hp10_cam_exp_t *cfg, scene_t *out)
{
    uint32_t period_ms;
    uint32_t frame_lines;
    float line_ms, exposure_ms, lin;
    scene_status_t clip = SCENE_OK;

    memset(out, 0, sizeof *out);
    out->status = SCENE_UNAVAILABLE;
    if (!luma || !live || !cfg)
        return;
    if (luma->avg >= 245)
        clip = SCENE_CLIPPED_HIGH;
    else if (luma->avg <= 3)
        clip = SCENE_CLIPPED_LOW;

    period_ms = hp10_cam_exp_measured_ms();
    if (period_ms == 0)
        period_ms = hp10_cam_exp_estimated_ms();
    frame_lines = (uint32_t)hp10_cam_exp_base_lines(live->fs) + cfg->dummy_lines;
    if (period_ms == 0 || frame_lines == 0)
        return;
    line_ms = (float)period_ms / (float)frame_lines;
    exposure_ms = (float)live->aec_value * line_ms;
    if (exposure_ms <= 0.0f)
        return;
    out->exposure_ms = exposure_ms;
    out->gain = hp10_cam_gain_multiplier(live->gain_reg);
    if (clip != SCENE_OK) {
        out->status = clip;
        return;
    }
    lin = (float)(luma->lin_mean > 0 ? luma->lin_mean : 1) / 65535.0f;
    out->ev = log2f(lin / ((exposure_ms / 1000.0f) * out->gain));
    out->status = SCENE_OK;
}

static void note_scene(uint32_t at_ms, const luma_t *luma, const scene_t *scene)
{
    lock();
    s_last_luma = *luma;
    s_have_last = true;
    s_last_sample_ms = at_ms;
    s_last_scene = *scene;
    s_have_scene = true;
    unlock();
}

/* One frame and one register read, taken back to back. Needs an OV2640. */
static bool sample_scene(uint32_t at_ms, luma_t *luma, scene_t *scene)
{
    hp10_cam_live_t live;
    hp10_cam_exp_t cfg;

    if (!sample_luma(luma))
        return false;
    if (!read_live(&live))
        return false;
    hp10_cam_exp_get(&cfg);
    scene_compute(luma, &live, &cfg, scene);
    note_scene(at_ms, luma, scene);
    return true;
}

/* ---- switching ---------------------------------------------------------- */

static void note_switch(hp10_cam_mode_t mode, bool ok, bool automatic, float ev,
                        const char *message)
{
    lock();
    s_last_switch.message = message;
    s_last_switch.mode = mode;
    s_last_switch.ok = ok;
    s_last_switch.automatic = automatic;
    s_last_switch.ev = ev;
    s_last_switch.ms = now_ms();
    s_last_switch.valid = true;
    if (ok) {
        s_last_switch_ms = s_last_switch.ms;
        s_have_last_switch = true;
    }
    unlock();
}

static void reset_counts(void)
{
    lock();
    s_low_count = 0;
    s_high_count = 0;
    unlock();
}

/* Validates, applies and saves one exposure. The previous one is put back
 * if the sensor refuses. */
static const char *apply_exposure(const hp10_cam_exp_t *cfg)
{
    hp10_cam_exp_t prev;
    const char *why;

    if (!g_cam_mu || xSemaphoreTake(g_cam_mu, pdMS_TO_TICKS(CAM_LOCK_MS)) != pdTRUE)
        return "Camera busy";
    if (!g_bCameraOk) {
        xSemaphoreGive(g_cam_mu);
        return "Camera not ready";
    }
    hp10_cam_exp_get(&prev);
    why = hp10_cam_exp_commit(cfg);
    if (why) {
        xSemaphoreGive(g_cam_mu);
        return why;
    }
    if (hp10_cam_exp_apply(hp10_cam_exp_active()) != 0) {
        why = hp10_cam_exp_last_error();
        if (!why)
            why = "Sensor rejected the settings";
        hp10_cam_exp_commit(&prev);
        hp10_cam_exp_apply(&prev);
        xSemaphoreGive(g_cam_mu);
        return why;
    }
    hp10_cam_exp_post_apply(hp10_cam_exp_active());
    xSemaphoreGive(g_cam_mu);
    hp10_cfg_save();
    return NULL;
}

const char *hp10_cam_auto_save_night(void)
{
    hp10_cam_exp_t cur;
    hp10_cam_exp_get(&cur);
    lock();
    s_night = cur;
    s_has_night = true;
    unlock();
    save_night(&cur);
    return NULL;
}

const char *hp10_cam_auto_apply(hp10_cam_mode_t mode, bool automatic, float trigger_ev)
{
    hp10_cam_exp_t night, cfg;
    const char *err;
    bool has;

    if (mode != HP10_MODE_DAY && mode != HP10_MODE_NIGHT)
        return "Unknown mode";
    lock();
    if (s_applying) {
        unlock();
        note_switch(mode, false, automatic, trigger_ev, "Switch already in progress");
        return "Switch already in progress";
    }
    s_applying = true;
    night = s_night;
    has = s_has_night;
    unlock();

    if (!has) {
        reset_counts();
        note_switch(mode, false, automatic, trigger_ev, "No night preset");
        lock();
        s_applying = false;
        unlock();
        return "No night preset";
    }
    if (mode == HP10_MODE_NIGHT)
        cfg = night;
    else
        hp10_cam_exp_day(&night, &cfg);
    err = apply_exposure(&cfg);
    if (err) {
        ESP_LOGW(TAG, "switch to %s failed: %s", mode_name(mode), err);
        reset_counts();
        note_switch(mode, false, automatic, trigger_ev, err);
    } else {
        lock();
        s_mode = mode;
        s_low_count = 0;
        s_high_count = 0;
        unlock();
        note_switch(mode, true, automatic, trigger_ev, NULL);
        save_mode(mode);
        ESP_LOGI(TAG, "switched to %s", mode_name(mode));
    }
    lock();
    s_applying = false;
    unlock();
    return err;
}

static const char *set_cfg(const asw_cfg_t *c)
{
    const char *err = validate_cfg(c);
    asw_cfg_t copy;
    if (err)
        return err;
    lock();
    s_cfg = *c;
    s_low_count = 0;
    s_high_count = 0;
    s_next_sample_ms = now_ms();
    copy = s_cfg;
    unlock();
    save_cfg(&copy);
    return NULL;
}

/* Nothing is sampled while someone is watching the stream or just took a
 * still, while an upgrade runs, or while the camera is down. */
static bool sampling_allowed(void)
{
    if (!g_bCameraOk || hp10_ota_busy())
        return false;
    return !hp10_www_cam_busy();
}

static void switching_service(uint32_t now)
{
    asw_cfg_t cfg;
    uint32_t next;
    bool have_last_switch;
    uint32_t last_switch_ms;
    hp10_cam_exp_t night;
    bool has_night;
    luma_t luma;
    scene_t scene;
    uint8_t low_count, high_count;
    bool to_night, to_day, dwell = false;
    hp10_cam_mode_t mode;
    bool is_low, is_high;
    char ev_text[16];
    float trigger_ev;

    lock();
    cfg = s_cfg;
    next = s_next_sample_ms;
    have_last_switch = s_have_last_switch;
    last_switch_ms = s_last_switch_ms;
    night = s_night;
    has_night = s_has_night;
    unlock();
    (void)night;
    if (!cfg.enabled || !has_night)
        return;
    if ((int32_t)(now - next) < 0)
        return;
    if (!sampling_allowed())
        return;

    lock();
    s_next_sample_ms = now + (uint32_t)cfg.interval_min * 60000UL;
    unlock();

    if (!hp10_cam_exp_is_ov2640()) {
        ESP_LOGW(TAG, "automatic switching needs an OV2640 sensor");
        return;
    }
    if (!sample_scene(now, &luma, &scene)) {
        ESP_LOGW(TAG, "scene sample failed");
        return;
    }
    if (scene.status == SCENE_UNAVAILABLE) {
        ESP_LOGW(TAG, "scene EV unavailable");
        return;
    }

    lock();
    mode = s_mode;
    is_low = (scene.status == SCENE_OK && scene.ev * 10.0f <= (float)cfg.low_ev_tenths) ||
             (scene.status == SCENE_CLIPPED_LOW && mode != HP10_MODE_NIGHT);
    is_high = (scene.status == SCENE_OK && scene.ev * 10.0f >= (float)cfg.high_ev_tenths) ||
              (scene.status == SCENE_CLIPPED_HIGH && mode != HP10_MODE_DAY);
    if (mode != HP10_MODE_NIGHT && is_low) {
        if (s_low_count < 255)
            s_low_count++;
    } else {
        s_low_count = 0;
    }
    if (mode != HP10_MODE_DAY && is_high) {
        if (s_high_count < 255)
            s_high_count++;
    } else {
        s_high_count = 0;
    }
    low_count = s_low_count;
    high_count = s_high_count;
    to_night = s_low_count >= cfg.samples;
    to_day = s_high_count >= cfg.samples;
    unlock();

    if ((to_night || to_day) && cfg.min_dwell_min > 0 && have_last_switch) {
        uint32_t ready_at = last_switch_ms + (uint32_t)cfg.min_dwell_min * 60000UL;
        dwell = (int32_t)(now - ready_at) < 0;
    }
    if (scene.status == SCENE_OK)
        snprintf(ev_text, sizeof ev_text, "%.1f", scene.ev);
    else
        snprintf(ev_text, sizeof ev_text, "-");
    ESP_LOGI(TAG, "luma avg=%u lin=%u scene=%s ev=%s mode=%s low=%u high=%u%s stack_free=%u",
             luma.avg, luma.lin_mean, scene_status_name(scene.status), ev_text,
             mode_name(mode), low_count, high_count, dwell ? " dwell" : "",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (dwell)
        return;
    trigger_ev = scene.status == SCENE_OK ? scene.ev : NAN;
    if (to_night)
        hp10_cam_auto_apply(HP10_MODE_NIGHT, true, trigger_ev);
    else if (to_day)
        hp10_cam_auto_apply(HP10_MODE_DAY, true, trigger_ev);
}

/* Stack headroom of every task, to size them against the tight internal RAM. */
static void log_task_stacks(void)
{
    static const char *names[] = {
        "cam_auto", "eco", "upload", "ota_poll", "systime", "sta_link", "ap_auto",
        "http_beat", "http_wd", "ws_log", "httpd", "tiT", "sys_evt", "wifi",
        "esp_timer", "mdns", "main",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        TaskHandle_t h = xTaskGetHandle(names[i]);
        if (h)
            ESP_LOGI(TAG, "stack %-10s free=%u", names[i],
                     (unsigned)(uxTaskGetStackHighWaterMark(h) * sizeof(StackType_t)));
    }
}

/* Keeps the scene EV on the page while switching is off, so thresholds can
 * be chosen from real readings. Never touches counters or the mode. */
static void telemetry_service(uint32_t now)
{
    luma_t luma;
    scene_t scene;
    hp10_cam_live_t live;

    /* Only while someone has the Camera page open: each sample holds the
     * camera for about a second and allocates a JPEG-sized buffer. */
    if (!s_polled || (int32_t)(now - s_last_poll_ms) > (int32_t)POLL_WINDOW_MS)
        return;
    if ((int32_t)(now - s_next_telemetry_ms) < 0 || !sampling_allowed())
        return;
    s_next_telemetry_ms = now + TELEMETRY_MS;
    if (!hp10_cam_exp_is_ov2640())
        return;
    if (!sample_scene(now, &luma, &scene))
        return;
    if (read_live(&live)) {
        ESP_LOGI(TAG, "live aec=%s val=%u agc=%s gain=0x%02X x%.2f est=%ums meas=%ums",
                 live.aec_on ? "on" : "off", live.aec_value, live.agc_on ? "on" : "off",
                 live.gain_reg, hp10_cam_gain_multiplier(live.gain_reg),
                 (unsigned)hp10_cam_exp_estimated_ms(), (unsigned)hp10_cam_exp_measured_ms());
    }
    {
        static uint32_t next_stk_ms;
        if ((int32_t)(now - next_stk_ms) >= 0) {
            next_stk_ms = now + 600000UL;
            log_task_stacks();
        }
    }
    if (scene.status == SCENE_OK)
        ESP_LOGI(TAG, "scene ev=%.1f exp=%.1fms gain=x%.2f luma=%u stack_free=%u int_free=%u int_min=%u", scene.ev,
                 scene.exposure_ms, scene.gain, luma.avg,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    else
        ESP_LOGI(TAG, "scene %s luma=%u", scene_status_name(scene.status), luma.avg);
}

static void auto_task(void *arg)
{
    (void)arg;
    for (;;) {
        hp10_cam_mode_t want;
        uint32_t now;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS));
        want = s_pending;
        if (want != HP10_MODE_UNKNOWN) {
            s_pending = HP10_MODE_UNKNOWN;
            hp10_cam_auto_apply(want, false, NAN);
        }
        if (s_settle_req) {
            s_settle_req = false;
            if (g_cam_mu && g_bCameraOk &&
                xSemaphoreTake(g_cam_mu, pdMS_TO_TICKS(15000)) == pdTRUE) {
                hp10_cam_exp_post_apply(NULL);
                xSemaphoreGive(g_cam_mu);
            }
        }
        now = now_ms();
        /* Let boot, the first stream and the first upload settle first. */
        if (now < 45000)
            continue;
        switching_service(now);
        telemetry_service(now);
    }
}

/* A manual switch runs here, not on the port 80 worker: settling can take
 * several frames and that worker must stay free. */
bool hp10_cam_auto_request_settle(void)
{
    if (!s_task)
        return false;
    s_settle_req = true;
    xTaskNotifyGive(s_task);
    return true;
}

static const char *queue_apply(hp10_cam_mode_t mode)
{
    bool has;
    lock();
    has = s_has_night;
    unlock();
    if (!has)
        return "No night preset";
    if (!s_task)
        return "Still starting up. Try again in a minute.";
    if (s_pending != HP10_MODE_UNKNOWN)
        return "Switch already in progress";
    s_pending = mode;
    xTaskNotifyGive(s_task);
    return NULL;
}

/* The stack comes out of internal RAM, which is at its lowest during boot
 * (the 23-byte low point was seen before 50 s). The task idles for its first
 * 45 s anyway, so it is only created once boot has settled. */
static esp_timer_handle_t s_start_timer;

static void start_task_cb(void *arg)
{
    (void)arg;
    if (xTaskCreate(auto_task, "cam_auto", TASK_STACK, NULL, 2, &s_task) != pdPASS) {
        ESP_LOGW(TAG, "task create failed, retrying in 30 s");
        esp_timer_start_once(s_start_timer, 30LL * 1000000LL);
    }
}

void hp10_cam_auto_start(void)
{
    if (s_mu)
        return;
    s_mu = xSemaphoreCreateMutex();
    if (!s_mu) {
        ESP_LOGE(TAG, "mutex failed, switching disabled");
        return;
    }
    load_all();
    s_next_telemetry_ms = now_ms() + TELEMETRY_MS;
    {
        const esp_timer_create_args_t args = { .callback = start_task_cb, .name = "auto_start" };
        if (esp_timer_create(&args, &s_start_timer) != ESP_OK ||
            esp_timer_start_once(s_start_timer, 50LL * 1000000LL) != ESP_OK)
            ESP_LOGE(TAG, "start timer failed, switching disabled");
    }
}

/* ---- web ---------------------------------------------------------------- */

static void add_exp_json(cJSON *o, const hp10_cam_exp_t *c)
{
    cJSON_AddBoolToObject(o, "aecOn", c->aec_on);
    cJSON_AddNumberToObject(o, "aecValue", c->aec_value);
    cJSON_AddBoolToObject(o, "aec2On", c->aec2);
    cJSON_AddNumberToObject(o, "aeLevel", c->ae_level);
    cJSON_AddBoolToObject(o, "agcOn", c->agc_on);
    cJSON_AddNumberToObject(o, "agcGain", c->agc_gain);
    cJSON_AddNumberToObject(o, "gainCeiling", c->gain_ceiling);
    cJSON_AddNumberToObject(o, "clkrc", c->clkrc);
    cJSON_AddNumberToObject(o, "dummyLines", c->dummy_lines);
}

void hp10_cam_auto_add_json(cJSON *o)
{
    asw_cfg_t cfg;
    hp10_cam_mode_t mode;
    uint8_t low_count, high_count;
    luma_t last;
    bool have_last;
    uint32_t last_sample_ms;
    scene_t scene;
    bool have_scene;
    switch_event_t sw;
    hp10_cam_exp_t night;
    bool has_night;
    uint32_t now = now_ms();

    if (!o)
        return;
    if (!s_mu) {
        cJSON_AddBoolToObject(o, "available", false);
        return;
    }
    lock();
    cfg = s_cfg;
    mode = s_mode;
    low_count = s_low_count;
    high_count = s_high_count;
    last = s_last_luma;
    have_last = s_have_last;
    last_sample_ms = s_last_sample_ms;
    scene = s_last_scene;
    have_scene = s_have_scene;
    sw = s_last_switch;
    night = s_night;
    has_night = s_has_night;
    unlock();

    cJSON_AddBoolToObject(o, "enabled", cfg.enabled);
    cJSON_AddNumberToObject(o, "lowEv", cfg.low_ev_tenths / 10.0);
    cJSON_AddNumberToObject(o, "highEv", cfg.high_ev_tenths / 10.0);
    cJSON_AddNumberToObject(o, "intervalMin", cfg.interval_min);
    cJSON_AddNumberToObject(o, "samples", cfg.samples);
    cJSON_AddNumberToObject(o, "minDwellMin", cfg.min_dwell_min);
    cJSON_AddStringToObject(o, "mode", mode_name(mode));
    cJSON_AddNumberToObject(o, "lowCount", low_count);
    cJSON_AddNumberToObject(o, "highCount", high_count);
    if (have_last) {
        cJSON *l = cJSON_AddObjectToObject(o, "lastLuma");
        cJSON_AddNumberToObject(l, "avg", last.avg);
        cJSON_AddNumberToObject(l, "min", last.min);
        cJSON_AddNumberToObject(l, "max", last.max);
        cJSON_AddNumberToObject(o, "lastSampleAgeS", (uint32_t)((now - last_sample_ms) / 1000UL));
    } else {
        cJSON_AddNullToObject(o, "lastLuma");
        cJSON_AddNullToObject(o, "lastSampleAgeS");
    }
    cJSON_AddBoolToObject(o, "available", hp10_cam_exp_is_ov2640());
    if (have_scene) {
        cJSON *sc = cJSON_AddObjectToObject(o, "lastScene");
        cJSON_AddStringToObject(sc, "status", scene_status_name(scene.status));
        if (scene.status == SCENE_OK)
            cJSON_AddNumberToObject(sc, "ev", scene.ev);
        else
            cJSON_AddNullToObject(sc, "ev");
        cJSON_AddNumberToObject(sc, "exposureMs", scene.exposure_ms);
        cJSON_AddNumberToObject(sc, "gain", scene.gain);
    } else {
        cJSON_AddNullToObject(o, "lastScene");
    }
    if (sw.valid && (sw.mode == HP10_MODE_DAY || sw.mode == HP10_MODE_NIGHT)) {
        cJSON *s = cJSON_AddObjectToObject(o, "lastSwitch");
        cJSON_AddBoolToObject(s, "ok", sw.ok);
        cJSON_AddStringToObject(s, "to", sw.mode == HP10_MODE_NIGHT ? "night" : "day");
        cJSON_AddStringToObject(s, "trigger", sw.automatic ? "auto" : "manual");
        if (!isnan(sw.ev))
            cJSON_AddNumberToObject(s, "ev", sw.ev);
        else
            cJSON_AddNullToObject(s, "ev");
        if (sw.message)
            cJSON_AddStringToObject(s, "message", sw.message);
        else
            cJSON_AddNullToObject(s, "message");
        cJSON_AddNumberToObject(s, "ageS", (uint32_t)((now - sw.ms) / 1000UL));
    } else {
        cJSON_AddNullToObject(o, "lastSwitch");
    }
    cJSON_AddBoolToObject(o, "hasNight", has_night);
    if (has_night)
        add_exp_json(cJSON_AddObjectToObject(o, "night"), &night);
    else
        cJSON_AddNullToObject(o, "night");
}

static esp_err_t auto_get(httpd_req_t *req)
{
    cJSON *o;
    uint32_t now = now_ms();
    if (guest_json(req))
        return ESP_OK;
    if (!s_polled || (int32_t)(now - s_last_poll_ms) > (int32_t)POLL_WINDOW_MS)
        s_next_telemetry_ms = now;
    s_last_poll_ms = now;
    s_polled = true;
    o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    hp10_cam_auto_add_json(o);
    return send_json(req, o);
}

static bool read_num(const cJSON *in, const char *key, double *out)
{
    const cJSON *it = cJSON_GetObjectItem(in, key);
    if (!cJSON_IsNumber(it))
        return false;
    *out = it->valuedouble;
    return true;
}

static esp_err_t auto_post(httpd_req_t *req)
{
    char *body;
    cJSON *in;
    cJSON *out;
    const cJSON *action;
    const char *err = NULL;
    const char *ok_msg = "OK";

    if (guest_json(req))
        return ESP_OK;
    body = recv_body(req);
    in = body ? cJSON_Parse(body) : NULL;
    free(body);
    out = cJSON_CreateObject();
    if (!in) {
        cJSON_AddBoolToObject(out, "ok", false);
        cJSON_AddStringToObject(out, "message", "Bad request");
        return send_json(req, out);
    }
    if (!s_mu) {
        cJSON_Delete(in);
        cJSON_AddBoolToObject(out, "ok", false);
        cJSON_AddStringToObject(out, "message", "Switching is not running");
        return send_json(req, out);
    }
    action = cJSON_GetObjectItem(in, "action");
    if (!cJSON_IsString(action) || !action->valuestring) {
        err = "Unknown action";
    } else if (strcmp(action->valuestring, "saveNight") == 0) {
        err = hp10_cam_auto_save_night();
        ok_msg = "Night preset saved";
    } else if (strcmp(action->valuestring, "applyNight") == 0) {
        err = queue_apply(HP10_MODE_NIGHT);
        ok_msg = "Applying night settings";
    } else if (strcmp(action->valuestring, "applyDay") == 0) {
        err = queue_apply(HP10_MODE_DAY);
        ok_msg = "Applying day settings";
    } else if (strcmp(action->valuestring, "settings") == 0) {
        double low_ev, high_ev, interval, samples, dwell;
        const cJSON *en = cJSON_GetObjectItem(in, "enabled");
        if (!cJSON_IsBool(en) || !read_num(in, "lowEv", &low_ev) ||
            !read_num(in, "highEv", &high_ev) || !read_num(in, "intervalMin", &interval) ||
            !read_num(in, "samples", &samples) || !read_num(in, "minDwellMin", &dwell)) {
            err = "Missing settings";
        } else {
            long low_t = lroundf((float)low_ev * 10.0f);
            long high_t = lroundf((float)high_ev * 10.0f);
            if (low_t < -200 || low_t > 200 || high_t < -200 || high_t > 200 || low_t + 10 > high_t) {
                err = "EV thresholds out of range";
            } else if (interval < 0 || interval > 255 || samples < 0 || samples > 255 ||
                       dwell < 0 || dwell > 255) {
                err = "Settings out of range";
            } else {
                asw_cfg_t c;
                c.enabled = cJSON_IsTrue(en);
                c.low_ev_tenths = (int16_t)low_t;
                c.high_ev_tenths = (int16_t)high_t;
                c.interval_min = (uint8_t)interval;
                c.samples = (uint8_t)samples;
                c.min_dwell_min = (uint8_t)dwell;
                err = set_cfg(&c);
                ok_msg = "Switching settings saved";
            }
        }
    } else {
        err = "Unknown action";
    }
    cJSON_Delete(in);
    cJSON_AddBoolToObject(out, "ok", err == NULL);
    cJSON_AddStringToObject(out, "message", err ? err : ok_msg);
    if (!err)
        hp10_cam_auto_add_json(out);
    return send_json(req, out);
}

void hp10_cam_auto_register(httpd_handle_t http)
{
    register_get(http, "/api/exposure/auto", auto_get);
    register_post(http, "/api/exposure/auto", auto_post);
}
