#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>

#include "cam_exp.h"
#include "hp10_bringup.h"
#include "sky_awb.h"

static const char *TAG = "cam_exp";

#define OV2640_PID_VAL     0x26
#define DUMMY_LINES_MAX    512
#define GAIN_CEIL_MAX      6
#define AGC_GAIN_MAX       30
/* Mirrors FB_GET_TIMEOUT in esp32-camera. A frame slower than this can
 * never be delivered, so settings that would need one are refused. */
#define FB_GET_TIMEOUT_MS  4000
#define BUDGET_MS          ((FB_GET_TIMEOUT_MS * 3) / 4)

static hp10_cam_exp_t s_active;
static hp10_cam_exp_t s_last_good;
static bool           s_saved;
static bool           s_last_good_valid;
static const char    *s_last_err;

static bool     s_have_defaults;
static bool     s_have_dummy;
static uint8_t  s_def_clkrc;
static uint16_t s_def_dummy;
static uint32_t s_measured_ms;
static uint32_t s_simple_at_measure;
static uint32_t s_base_period_ms[FRAMESIZE_INVALID];

void hp10_cam_exp_init(void)
{
    hp10_cam_exp_defaults(&s_active);
    s_saved = false;
    s_last_good_valid = false;
}

static int clamp_i(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static uint16_t cur_pid(void)
{
    sensor_t *s = g_bCameraOk ? esp_camera_sensor_get() : NULL;
    return s ? s->id.PID : OV2640_PID_VAL;
}

static framesize_t cur_fs(void)
{
    sensor_t *s = g_bCameraOk ? esp_camera_sensor_get() : NULL;
    return s ? s->status.framesize : FRAMESIZE_UXGA;
}

bool hp10_cam_exp_is_ov2640(void)
{
    return g_bCameraOk && cur_pid() == OV2640_PID_VAL;
}

uint16_t hp10_cam_exp_base_lines(framesize_t fs)
{
    if ((int)fs <= (int)FRAMESIZE_CIF)
        return 336;
    if ((int)fs <= (int)FRAMESIZE_SVGA)
        return 672;
    return 1248;
}

static int ae_level_min(uint16_t pid) { return pid == OV2640_PID_VAL ? -2 : -5; }
static int ae_level_max(uint16_t pid) { return pid == OV2640_PID_VAL ? 2 : 5; }

static uint16_t aec_value_max(const hp10_cam_exp_t *cfg, uint16_t pid, framesize_t fs)
{
    uint32_t lines;
    if (pid != OV2640_PID_VAL)
        return 65535;
    lines = (uint32_t)hp10_cam_exp_base_lines(fs) + (cfg ? cfg->dummy_lines : 0);
    if (lines > 65535)
        lines = 65535;
    return lines ? (uint16_t)lines : 1;
}

static bool clkrc_allowed(uint8_t v)
{
    if (v == HP10_EXP_SENTINEL)
        return true;
    return v == 0x01 || v == 0x03 || v == 0x07 || v == 0x0F;
}

/* Same arithmetic as the sensor: a CLKRC write scales the frame period by
 * (new divider) / (default divider). */
static void period_scale(uint8_t user_clkrc, uint32_t *num, uint32_t *den)
{
    uint8_t applied = user_clkrc == HP10_EXP_SENTINEL ? s_def_clkrc : user_clkrc;
    uint32_t d_mul = (s_def_clkrc & 0x80) ? 2u : 1u;
    uint32_t u_mul = (applied & 0x80) ? 2u : 1u;
    uint32_t d_div = (uint32_t)(s_def_clkrc & 0x3F) + 1u;
    uint32_t u_div = (uint32_t)(applied & 0x3F) + 1u;
    *num = d_mul * u_div;
    *den = u_mul * d_div;
    if (*den == 0)
        *den = 1;
}

static uint32_t simple_estimate_ms(const hp10_cam_exp_t *cfg)
{
    if (!cfg->aec_on)
        return 80 + ((uint32_t)cfg->aec_value * 2500UL) / 1200UL;
    if (cfg->aec2)
        return 350;
    return 120;
}

static uint32_t ov2640_estimate_ms(const hp10_cam_exp_t *cfg, framesize_t fs)
{
    uint32_t base = hp10_cam_exp_base_lines(fs);
    uint32_t lines = base + cfg->dummy_lines;
    uint32_t num = 1, den = 1;
    uint32_t base_period = ((int)fs >= 0 && fs < FRAMESIZE_INVALID) ? s_base_period_ms[fs] : 0;
    uint64_t ms;

    if (!cfg->aec_on && cfg->aec_value > lines)
        lines = cfg->aec_value;
    period_scale(cfg->clkrc, &num, &den);
    if (base_period == 0) {
        uint32_t tline = 1922;
        uint32_t xclk = 16000000;
        sensor_t *s = g_bCameraOk ? esp_camera_sensor_get() : NULL;
        if ((int)fs <= (int)FRAMESIZE_CIF)
            tline = 595;
        else if ((int)fs <= (int)FRAMESIZE_SVGA)
            tline = 1190;
        if (s && s->xclk_freq_hz > 1000000)
            xclk = (uint32_t)s->xclk_freq_hz;
        base_period = (uint32_t)(((uint64_t)base * tline * 1000ULL) / xclk);
        if (base_period == 0)
            base_period = 1;
    }
    if (base == 0 || den == 0)
        return base_period;
    ms = (uint64_t)base_period * num * lines / ((uint64_t)den * base);
    if (ms == 0)
        return 1;
    return ms > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (uint32_t)ms;
}

static uint32_t estimate_ms(const hp10_cam_exp_t *cfg, uint16_t pid, framesize_t fs)
{
    uint32_t simple;
    if (pid == OV2640_PID_VAL)
        return ov2640_estimate_ms(cfg, fs);
    simple = simple_estimate_ms(cfg);
    if (s_measured_ms > 0 && s_simple_at_measure > 0)
        return (uint32_t)((uint64_t)s_measured_ms * simple / s_simple_at_measure);
    return simple;
}

uint32_t hp10_cam_exp_estimated_ms(void)
{
    return estimate_ms(&s_active, cur_pid(), cur_fs());
}

uint32_t hp10_cam_exp_measured_ms(void)
{
    return s_measured_ms;
}

static void note_measured(uint32_t period_ms, const hp10_cam_exp_t *cfg, uint16_t pid,
                          framesize_t fs)
{
    s_measured_ms = period_ms;
    if (period_ms == 0 || !cfg)
        return;
    if (pid == OV2640_PID_VAL) {
        uint32_t base = hp10_cam_exp_base_lines(fs);
        uint32_t lines = base + cfg->dummy_lines;
        uint32_t num = 1, den = 1;
        uint64_t baseline;
        if (!cfg->aec_on && cfg->aec_value > lines)
            lines = cfg->aec_value;
        period_scale(cfg->clkrc, &num, &den);
        if ((int)fs < 0 || fs >= FRAMESIZE_INVALID || num == 0 || lines == 0 || base == 0)
            return;
        baseline = (uint64_t)period_ms * den * base / ((uint64_t)num * lines);
        s_base_period_ms[fs] = baseline ? (uint32_t)baseline : 1;
    } else {
        s_simple_at_measure = simple_estimate_ms(cfg);
    }
}

void hp10_cam_exp_defaults(hp10_cam_exp_t *out)
{
    if (!out)
        return;
    out->aec_on = 1;
    out->aec_value = 600;
    /* The sensor's own AEC2 metering exposes daylight scenes far better. */
    out->aec2 = 1;
    out->ae_level = 0;
    out->agc_on = 1;
    out->agc_gain = 15;
    out->gain_ceiling = HP10_EXP_SENTINEL;
    out->clkrc = HP10_EXP_SENTINEL;
    out->dummy_lines = 0;
}

void hp10_cam_exp_day(const hp10_cam_exp_t *base, hp10_cam_exp_t *out)
{
    *out = *base;
    out->aec_on = 1;
    out->aec2 = 1;
    out->agc_on = 1;
    out->clkrc = HP10_EXP_SENTINEL;
    out->dummy_lines = 0;
}

static const char *validate_for(const hp10_cam_exp_t *in, hp10_cam_exp_t *out,
                                uint16_t pid, framesize_t fs)
{
    static char msg[96];
    hp10_cam_exp_t t;
    uint32_t est;

    if (!in || !out)
        return "invalid exposure settings";
    t = *in;
    t.aec_on = t.aec_on ? 1 : 0;
    t.agc_on = t.agc_on ? 1 : 0;
    t.aec2 = t.aec2 ? 1 : 0;
    if (t.dummy_lines > DUMMY_LINES_MAX)
        return "dummyLines out of range";
    if (t.aec_value > aec_value_max(&t, pid, fs))
        return "Manual exposure is above the frame length. Add dummy lines or lower it.";
    if (t.ae_level < ae_level_min(pid) || t.ae_level > ae_level_max(pid))
        return "aeLevel out of range";
    if (t.agc_gain > AGC_GAIN_MAX)
        return "agcGain out of range";
    if (t.gain_ceiling != HP10_EXP_SENTINEL && t.gain_ceiling > GAIN_CEIL_MAX)
        return "gainCeiling out of range";
    if (!clkrc_allowed(t.clkrc))
        return "clkrc not allowed";
    est = estimate_ms(&t, pid, fs);
    if (est > BUDGET_MS) {
        snprintf(msg, sizeof msg, "Exposure timing too long (%u ms estimate, limit %u ms)",
                 (unsigned)est, (unsigned)BUDGET_MS);
        return msg;
    }
    *out = t;
    return NULL;
}

const char *hp10_cam_exp_validate(const hp10_cam_exp_t *in, hp10_cam_exp_t *out)
{
    return validate_for(in, out, cur_pid(), cur_fs());
}

bool hp10_cam_exp_saved(void)
{
    return s_saved;
}

const hp10_cam_exp_t *hp10_cam_exp_active(void)
{
    return &s_active;
}

void hp10_cam_exp_get(hp10_cam_exp_t *out)
{
    if (out)
        *out = s_active;
}

static const char *assign_active(const hp10_cam_exp_t *in, bool commit)
{
    hp10_cam_exp_t v;
    const char *why = hp10_cam_exp_validate(in, &v);
    if (why)
        return why;
    s_active = v;
    if (commit)
        s_saved = true;
    return NULL;
}

const char *hp10_cam_exp_update(const hp10_cam_exp_t *in)
{
    return assign_active(in, false);
}

const char *hp10_cam_exp_commit(const hp10_cam_exp_t *in)
{
    return assign_active(in, true);
}

static int json_val(const cJSON *it)
{
    if (!it)
        return 0;
    if (cJSON_IsString(it) && it->valuestring)
        return atoi(it->valuestring);
    if (cJSON_IsNumber(it))
        return it->valueint;
    return 0;
}

void hp10_cam_exp_patch_json(const cJSON *o, hp10_cam_exp_t *draft)
{
    const cJSON *it;
    if (!o || !draft)
        return;
    it = cJSON_GetObjectItem(o, "aec_sensor");
    if (it)
        draft->aec_on = json_val(it) ? 1 : 0;
    it = cJSON_GetObjectItem(o, "agc");
    if (it)
        draft->agc_on = json_val(it) ? 1 : 0;
    it = cJSON_GetObjectItem(o, "exposure");
    if (it)
        draft->aec_value = (uint16_t)clamp_i(json_val(it), 0, 65535);
    it = cJSON_GetObjectItem(o, "aec_dsp");
    if (it)
        draft->aec2 = json_val(it) ? 1 : 0;
    it = cJSON_GetObjectItem(o, "ae_level");
    if (it)
        draft->ae_level = (int8_t)clamp_i(json_val(it), -128, 127);
    it = cJSON_GetObjectItem(o, "agc_gain");
    if (it)
        draft->agc_gain = (uint8_t)clamp_i(json_val(it), 0, 255);
    it = cJSON_GetObjectItem(o, "gain_ceiling");
    if (it)
        draft->gain_ceiling = (uint8_t)clamp_i(json_val(it), 0, 255);
    it = cJSON_GetObjectItem(o, "clkrc");
    if (it) {
        int v = clamp_i(json_val(it), 0, 255);
        /* An older page sends 0 for "driver default". */
        draft->clkrc = (uint8_t)(v == 0 ? HP10_EXP_SENTINEL : v);
    }
    it = cJSON_GetObjectItem(o, "dummy_lines");
    if (it)
        draft->dummy_lines = (uint16_t)clamp_i(json_val(it), 0, 65535);
}

void hp10_cam_exp_add_json(cJSON *o)
{
    uint16_t pid = cur_pid();
    framesize_t fs = cur_fs();
    bool ov = g_bCameraOk && pid == OV2640_PID_VAL;
    cJSON *by;
    char key[8];

    if (!o)
        return;
    cJSON_AddNumberToObject(o, "aec_sensor", s_active.aec_on);
    cJSON_AddNumberToObject(o, "aec_dsp", s_active.aec2);
    cJSON_AddNumberToObject(o, "ae_level", s_active.ae_level);
    cJSON_AddNumberToObject(o, "exposure", s_active.aec_value);
    cJSON_AddNumberToObject(o, "agc", s_active.agc_on);
    cJSON_AddNumberToObject(o, "agc_gain", s_active.agc_gain);
    cJSON_AddNumberToObject(o, "gain_ceiling", s_active.gain_ceiling);
    cJSON_AddNumberToObject(o, "clkrc", s_active.clkrc);
    cJSON_AddNumberToObject(o, "dummy_lines", s_active.dummy_lines);

    cJSON_AddNumberToObject(o, "est_frame_ms", hp10_cam_exp_estimated_ms());
    cJSON_AddNumberToObject(o, "meas_frame_ms", s_measured_ms);
    cJSON_AddNumberToObject(o, "aec_max", aec_value_max(&s_active, pid, fs));
    cJSON_AddNumberToObject(o, "ae_level_min", ae_level_min(pid));
    cJSON_AddNumberToObject(o, "ae_level_max", ae_level_max(pid));
    cJSON_AddNumberToObject(o, "gain_ceiling_default",
                            ov ? 3 : (g_bCameraOk && pid == OV5640_PID ? 2 : -1));
    cJSON_AddNumberToObject(o, "supports_timing", ov ? 1 : 0);
    by = cJSON_AddObjectToObject(o, "aec_base_lines");
    for (int f = 0; f <= (int)FRAMESIZE_UXGA; f++) {
        snprintf(key, sizeof key, "%d", f);
        cJSON_AddNumberToObject(by, key, hp10_cam_exp_base_lines((framesize_t)f));
    }
}

static bool load_u8(nvs_handle_t h, const char *key, uint8_t *dst, int lo, int hi)
{
    uint8_t v = 0;
    if (nvs_get_u8(h, key, &v) != ESP_OK)
        return false;
    if ((int)v < lo || (int)v > hi)
        return false;
    *dst = v;
    return true;
}

void hp10_cam_exp_load(nvs_handle_t h)
{
    hp10_cam_exp_defaults(&s_active);
    s_saved = false;
    uint8_t set = 0;
    if (nvs_get_u8(h, "cam_exp_set", &set) != ESP_OK || set != 1)
        return;

    hp10_cam_exp_t t;
    hp10_cam_exp_defaults(&t);
    if (!load_u8(h, "cam_aec", &t.aec_on, 0, 1))
        return;
    if (!load_u8(h, "cam_aec2", &t.aec2, 0, 1))
        return;
    if (!load_u8(h, "cam_agc", &t.agc_on, 0, 1))
        return;
    if (!load_u8(h, "cam_agc_g", &t.agc_gain, 0, AGC_GAIN_MAX))
        return;
    if (!load_u8(h, "cam_gceil", &t.gain_ceiling, 0, 255))
        return;
    if (!load_u8(h, "cam_clkrc", &t.clkrc, 0, 255))
        return;
    /* Older builds stored 0 for "driver default". */
    if (t.clkrc == 0)
        t.clkrc = HP10_EXP_SENTINEL;
    {
        int8_t ael = 0;
        if (nvs_get_i8(h, "cam_ael", &ael) != ESP_OK)
            return;
        t.ae_level = ael;
    }
    {
        uint16_t av = 0;
        if (nvs_get_u16(h, "cam_aec_v", &av) != ESP_OK)
            return;
        t.aec_value = av;
    }
    {
        uint16_t dl = 0;
        if (nvs_get_u16(h, "cam_dummy", &dl) != ESP_OK || dl > DUMMY_LINES_MAX)
            return;
        t.dummy_lines = dl;
    }
    /* The camera is not up yet, so check against the largest frame. The
     * real limit is applied when the value reaches the sensor. */
    if (validate_for(&t, &s_active, OV2640_PID_VAL, FRAMESIZE_UXGA) != NULL)
        hp10_cam_exp_defaults(&s_active);
    else
        s_saved = true;
    ESP_LOGI(TAG, "nvs aec=%u agc=%u clk=0x%02X dummy=%u",
             s_active.aec_on, s_active.agc_on,
             s_active.clkrc, s_active.dummy_lines);
}

void hp10_cam_exp_save(nvs_handle_t h)
{
    if (!s_saved)
        return;
    nvs_set_u8(h, "cam_exp_set", 1);
    nvs_set_u8(h, "cam_aec", s_active.aec_on);
    nvs_set_u16(h, "cam_aec_v", s_active.aec_value);
    nvs_set_u8(h, "cam_aec2", s_active.aec2);
    nvs_set_i8(h, "cam_ael", s_active.ae_level);
    nvs_set_u8(h, "cam_agc", s_active.agc_on);
    nvs_set_u8(h, "cam_agc_g", s_active.agc_gain);
    nvs_set_u8(h, "cam_gceil", s_active.gain_ceiling);
    nvs_set_u8(h, "cam_clkrc", s_active.clkrc);
    nvs_set_u16(h, "cam_dummy", s_active.dummy_lines);
}

static bool ov2640_sensor(sensor_t *s)
{
    return s && s->id.PID == OV2640_PID_VAL;
}

static int read_reg(sensor_t *s, int reg)
{
    if (!s || !s->get_reg)
        return -1;
    return s->get_reg(s, reg, 0xFF);
}

void hp10_cam_exp_forget_defaults(void)
{
    s_have_defaults = false;
    s_have_dummy = false;
}

void hp10_cam_exp_capture_defaults(void)
{
    sensor_t *s = g_bCameraOk ? esp_camera_sensor_get() : NULL;
    int clk, lo, hi;
    if (!ov2640_sensor(s))
        return;
    clk = read_reg(s, 0x0111);
    lo = read_reg(s, 0x012D);
    hi = read_reg(s, 0x012E);
    if (clk < 0 || lo < 0 || hi < 0) {
        ESP_LOGW(TAG, "ov2640 register read failed");
        return;
    }
    s_def_clkrc = (uint8_t)clk;
    /* set_framesize rewrites CLKRC and leaves the dummy-line registers
     * alone, so their defaults come only from the first read after init. */
    if (!s_have_dummy) {
        s_def_dummy = (uint16_t)(lo & 0xFF) | ((uint16_t)(hi & 0xFF) << 8);
        s_have_dummy = true;
    }
    s_have_defaults = true;
}

static const char *apply_timing(sensor_t *s, uint8_t clkrc, uint16_t dummy_lines)
{
    static const char *rejected = "Sensor rejected clock or dummy-line settings";
    uint8_t clk;
    uint16_t dummy;
    if (!s_have_defaults || !s_have_dummy)
        hp10_cam_exp_capture_defaults();
    if (!s_have_defaults || !s_have_dummy)
        return "OV2640 timing defaults were not captured";
    if (!s->set_reg)
        return rejected;
    clk = clkrc == HP10_EXP_SENTINEL ? s_def_clkrc : clkrc;
    dummy = dummy_lines == 0 ? s_def_dummy : dummy_lines;
    if (s->set_reg(s, 0x0111, 0xFF, clk) != 0 ||
        s->set_reg(s, 0x012D, 0xFF, (int)(dummy & 0xFF)) != 0 ||
        s->set_reg(s, 0x012E, 0xFF, (int)((dummy >> 8) & 0xFF)) != 0)
        return rejected;
    return NULL;
}

static int ov2640_set_aec(sensor_t *s, int value)
{
    if (!s->set_reg)
        return -1;
    value = clamp_i(value, 0, 65535);
    if (s->set_reg(s, 0x0104, 0x03, value & 0x03) != 0)
        return -1;
    if (s->set_reg(s, 0x0110, 0xFF, (value >> 2) & 0xFF) != 0)
        return -1;
    if (s->set_reg(s, 0x0145, 0x3F, (value >> 10) & 0x3F) != 0)
        return -1;
    return 0;
}

float hp10_cam_gain_multiplier(uint8_t gain_reg)
{
    unsigned coarse = 1;
    if (gain_reg & 0x10)
        coarse *= 2;
    if (gain_reg & 0x20)
        coarse *= 2;
    if (gain_reg & 0x40)
        coarse *= 2;
    if (gain_reg & 0x80)
        coarse *= 2;
    return (float)(coarse * (16u + (unsigned)(gain_reg & 0x0F))) / 16.0f;
}

bool hp10_cam_exp_read_live(hp10_cam_live_t *out)
{
    sensor_t *s = g_bCameraOk ? esp_camera_sensor_get() : NULL;
    int com8, reg45, aec, reg04, gain, com9;
    if (!s || !out || !ov2640_sensor(s))
        return false;
    com8 = read_reg(s, 0x0113);
    reg45 = read_reg(s, 0x0145);
    aec = read_reg(s, 0x0110);
    reg04 = read_reg(s, 0x0104);
    gain = read_reg(s, 0x0100);
    com9 = read_reg(s, 0x0114);
    if (com8 < 0 || reg45 < 0 || aec < 0 || reg04 < 0 || gain < 0 || com9 < 0)
        return false;
    out->aec_on = (com8 & 0x01) != 0;
    out->agc_on = (com8 & 0x04) != 0;
    out->aec_value = (uint16_t)(((reg45 & 0x3F) << 10) | ((aec & 0xFF) << 2) | (reg04 & 0x03));
    out->gain_reg = (uint8_t)gain;
    out->ceil_idx = (uint8_t)((com9 >> 5) & 0x07);
    out->pid = s->id.PID;
    out->fs = s->status.framesize;
    return true;
}

static gainceiling_t gain_ceiling_for(uint8_t idx, uint16_t pid, bool *set)
{
    *set = true;
    if (idx != HP10_EXP_SENTINEL)
        return (gainceiling_t)(idx > GAIN_CEIL_MAX ? GAIN_CEIL_MAX : idx);
    if (pid == OV2640_PID_VAL)
        return GAINCEILING_16X;
    if (pid == OV5640_PID)
        return GAINCEILING_8X;
    *set = false;
    return GAINCEILING_2X;
}

const char *hp10_cam_exp_last_error(void)
{
    return s_last_err;
}

int hp10_cam_exp_apply(const hp10_cam_exp_t *cfg)
{
    sensor_t *s;
    uint16_t pid;
    framesize_t fs;

    if (!cfg || !g_bCameraOk) {
        s_last_err = "Camera not ready";
        return -1;
    }
    s = esp_camera_sensor_get();
    if (!s) {
        s_last_err = "Camera not ready";
        return -1;
    }
    pid = s->id.PID;
    fs = s->status.framesize;

    if (pid == OV2640_PID_VAL) {
        const char *err = apply_timing(s, cfg->clkrc, cfg->dummy_lines);
        if (err) {
            ESP_LOGW(TAG, "%s", err);
            s_last_err = err;
            return -1;
        }
    }

    if (cfg->aec_on) {
        if (s->set_exposure_ctrl)
            s->set_exposure_ctrl(s, 1);
        if (s->set_aec2)
            s->set_aec2(s, cfg->aec2 ? 1 : 0);
        if (s->set_ae_level)
            s->set_ae_level(s, cfg->ae_level);
    } else {
        int v = cfg->aec_value;
        int mx = aec_value_max(cfg, pid, fs);
        if (s->set_exposure_ctrl)
            s->set_exposure_ctrl(s, 0);
        if (v > mx)
            v = mx;
        if (pid == OV2640_PID_VAL) {
            if (ov2640_set_aec(s, v) != 0) {
                ESP_LOGW(TAG, "manual exposure rejected");
                s_last_err = "Sensor rejected manual exposure";
                return -1;
            }
            s->status.aec_value = (uint16_t)v;
        } else if (s->set_aec_value) {
            s->set_aec_value(s, v);
        }
    }

    if (cfg->agc_on) {
        bool set;
        gainceiling_t gc = gain_ceiling_for(cfg->gain_ceiling, pid, &set);
        if (s->set_gain_ctrl)
            s->set_gain_ctrl(s, 1);
        if (set && s->set_gainceiling)
            s->set_gainceiling(s, gc);
    } else {
        if (s->set_gain_ctrl)
            s->set_gain_ctrl(s, 0);
        if (s->set_agc_gain)
            s->set_agc_gain(s, cfg->agc_gain);
    }

    hp10_sky_awb_apply();
    ESP_LOGI(TAG, "apply aec=%u val=%u aec2=%u ae=%d agc=%u gain=%u ceil=0x%02X clk=0x%02X dummy=%u",
             cfg->aec_on, cfg->aec_value, cfg->aec2, cfg->ae_level, cfg->agc_on,
             cfg->agc_gain, cfg->gain_ceiling, cfg->clkrc, cfg->dummy_lines);
    s_last_err = NULL;
    return 0;
}

/* Drop frames until the new exposure has reached the output, and time two
 * fresh ones to learn the real frame period. Returns fresh frames seen. */
static int settle(const hp10_cam_exp_t *cfg)
{
    bool manual = !cfg->aec_on && !cfg->agc_on;
    int min_frames = manual ? 2 : 3;
    int min_ms = manual ? 0 : 1500;
    const int frame_cap = 10;
    int64_t started = esp_timer_get_time() / 1000;
    int64_t ts[10];
    int fresh = 0;
    int attempts = 0;
    sensor_t *s;

    note_measured(0, cfg, 0, FRAMESIZE_SVGA);
    while (fresh < frame_cap && attempts < frame_cap &&
           (esp_timer_get_time() / 1000 - started) < (int64_t)BUDGET_MS * 3) {
        camera_fb_t *fb = esp_camera_fb_get();
        int64_t t;
        attempts++;
        if (!fb)
            break;
        t = (int64_t)fb->timestamp.tv_sec * 1000 + fb->timestamp.tv_usec / 1000;
        esp_camera_fb_return(fb);
        if (started - t > 50)
            continue;
        ts[fresh++] = t;
        if (fresh >= min_frames && (esp_timer_get_time() / 1000 - started) >= min_ms)
            break;
    }
    if (fresh >= 2 && ts[fresh - 1] > ts[fresh - 2]) {
        s = esp_camera_sensor_get();
        note_measured((uint32_t)(ts[fresh - 1] - ts[fresh - 2]), cfg,
                      s ? s->id.PID : 0, s ? s->status.framesize : FRAMESIZE_SVGA);
    } else {
        ESP_LOGW(TAG, "frame period not measured: fresh=%d attempts=%d", fresh, attempts);
    }
    return fresh;
}

void hp10_cam_exp_post_apply(const hp10_cam_exp_t *applied)
{
    int fresh;

    if (!applied)
        applied = &s_active;
    fresh = settle(applied);
    if (fresh == 0 && s_last_good_valid) {
        ESP_LOGW(TAG, "no frame after apply, revert last good");
        s_active = s_last_good;
        hp10_cam_exp_apply(&s_active);
        settle(&s_active);
    } else if (fresh > 0) {
        s_last_good = *applied;
        s_last_good_valid = true;
    }
}

void hp10_cam_exp_apply_locked(const hp10_cam_exp_t *cfg)
{
    if (!cfg)
        return;
    if (g_cam_mu && xSemaphoreTake(g_cam_mu, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGW(TAG, "camera busy, exposure not applied");
        return;
    }
    hp10_cam_exp_apply(cfg);
    hp10_cam_exp_post_apply(cfg);
    if (g_cam_mu)
        xSemaphoreGive(g_cam_mu);
}

void hp10_cam_exp_apply_active_locked(void)
{
    hp10_cam_exp_apply_locked(&s_active);
}
