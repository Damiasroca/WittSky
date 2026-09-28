#include "esp_camera.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#include "hp10_bringup.h"
#include "sky_awb.h"
#include "sky_cfg.h"
#include "sky_sensor.h"
#include "sky_stats.h"

static const char *TAG = "awb_match";

#define SETTLE_FRAMES 50
#define AVG_FRAMES    4
#define SKIP_FRAMES   10
#define MAX_ROUNDS    12
#define MATCH_TOL     2.0
#define CHECK_TOL     4.0

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_state;
static char s_msg[80];
static char s_name[SKY_AWB_NAME_MAX + 1];

static void set_status(int state, const char *msg)
{
    char tmp[sizeof s_msg];
    snprintf(tmp, sizeof tmp, "%s", msg ? msg : "");
    portENTER_CRITICAL(&s_mux);
    s_state = state;
    memcpy(s_msg, tmp, sizeof s_msg);
    portEXIT_CRITICAL(&s_mux);
}

bool hp10_sky_awb_cal_busy(void)
{
    bool busy;
    portENTER_CRITICAL(&s_mux);
    busy = s_state == 1;
    portEXIT_CRITICAL(&s_mux);
    return busy;
}

void hp10_sky_awb_cal_status(char *state, size_t state_n, char *msg, size_t msg_n)
{
    int st;
    char tmp[sizeof s_msg];
    portENTER_CRITICAL(&s_mux);
    st = s_state;
    memcpy(tmp, s_msg, sizeof tmp);
    portEXIT_CRITICAL(&s_mux);
    const char *name = "idle";
    if (st == 1)
        name = "run";
    else if (st == 2)
        name = "ok";
    else if (st == 3)
        name = "fail";
    if (state && state_n)
        snprintf(state, state_n, "%s", name);
    if (msg && msg_n)
        snprintf(msg, msg_n, "%s", tmp);
}

static bool near(double a, double b, double tol)
{
    double d = a - b;
    if (d < 0)
        d = -d;
    return d <= tol;
}

static double chan_gap(double a, double b)
{
    double d = a - b;
    if (d < 0)
        d = -d;
    return d;
}

/* Half of the full correction. A full step overshoots this sensor and the
 * search oscillates around the target instead of settling on it. */
static int step_gain(int gain, double target, double measured)
{
    if (measured < 1.0)
        return -1;
    double ideal = (double)gain * target / measured;
    if (ideal < 1.0)
        ideal = 1.0;
    if (ideal > 255.0)
        ideal = 255.0;
    double next = (double)gain + 0.5 * (ideal - (double)gain);
    int n = (int)(next + 0.5);
    if (n == gain && chan_gap(measured, target) > MATCH_TOL) {
        if (ideal > (double)gain)
            n = gain + 1;
        else if (ideal < (double)gain)
            n = gain - 1;
    }
    if (n < 1)
        n = 1;
    if (n > 255)
        n = 255;
    return n;
}

static bool discard_frames(int n)
{
    for (int i = 0; i < n; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb)
            return false;
        esp_camera_fb_return(fb);
        hp10_wd_note_camera_ok();
    }
    return true;
}

static bool average_mask(double *r, double *g, double *b)
{
    double sr = 0, sg = 0, sb = 0;
    for (int i = 0; i < AVG_FRAMES; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb)
            return false;
        double rr, gg, bb;
        bool ok = hp10_sky_mask_mean(fb->buf, fb->len, &rr, &gg, &bb);
        esp_camera_fb_return(fb);
        hp10_wd_note_camera_ok();
        if (!ok)
            return false;
        sr += rr;
        sg += gg;
        sb += bb;
    }
    *r = sr / AVG_FRAMES;
    *g = sg / AVG_FRAMES;
    *b = sb / AVG_FRAMES;
    return true;
}

static bool auto_wb(sensor_t *s)
{
    return s->set_whitebal(s, 1) == 0 &&
           s->set_awb_gain(s, 1) == 0 &&
           s->set_wb_mode(s, 0) == 0;
}

static bool manual_wb(sensor_t *s, int r, int g, int b)
{
    return s->set_whitebal(s, 1) == 0 &&
           s->set_awb_gain(s, 1) == 0 &&
           s->set_reg(s, 0x0C7, 0x40, 0x40) == 0 &&
           s->set_reg(s, 0x0CC, 0xFF, r) == 0 &&
           s->set_reg(s, 0x0CD, 0xFF, g) == 0 &&
           s->set_reg(s, 0x0CE, 0xFF, b) == 0;
}

static void unfreeze(sensor_t *s)
{
    if (s && s->set_exposure_ctrl && s->set_gain_ctrl) {
        s->set_exposure_ctrl(s, 1);
        s->set_gain_ctrl(s, 1);
    }
}

static bool freeze_exposure(sensor_t *s)
{
    sky_sensor_t sen;
    sky_sensor_read(1.0, &sen);
    if (!sen.read_ok || !s->set_exposure_ctrl || !s->set_gain_ctrl || !s->set_aec_value || !s->set_reg)
        return false;
    if (s->set_exposure_ctrl(s, 0) != 0 || s->set_gain_ctrl(s, 0) != 0)
        return false;
    if (s->set_aec_value(s, sen.aec) != 0 || s->set_reg(s, 0x100, 0xFF, sen.gain_reg) != 0)
        return false;
    return true;
}

static void cal_task(void *arg)
{
    (void)arg;
    sensor_t *s = esp_camera_sensor_get();
    bool held = false;
    bool matched = false;
    int round = 0;
    int gr = 0x40, gg = 0x40, gb = 0x40;
    int br = gr, bg = gg, bb = gb;
    double berr = 1e9;
    double tr = 0, tg = 0, tb = 0, mr = 0, mg = 0, mb = 0, cr = 0, cg = 0, cb = 0;
    double bmr = 0, bmg = 0, bmb = 0;
    const char *why = NULL;
    const char *fail = "Could not calibrate";
    char okmsg[sizeof s_msg];

    if (!g_cam_mu || !g_bCameraOk || !s) {
        set_status(3, "Camera is not ready");
        vTaskDelete(NULL);
        return;
    }

    xSemaphoreTake(g_cam_mu, portMAX_DELAY);
    held = true;

    set_status(1, "Settling automatic white balance");
    if (!auto_wb(s) || !discard_frames(SETTLE_FRAMES) || !freeze_exposure(s)) {
        fail = "Could not settle automatic white balance";
        goto done;
    }

    set_status(1, "Measuring the sky mask");
    if (!average_mask(&tr, &tg, &tb)) {
        fail = "The sky mask is empty or a frame was missed";
        goto done;
    }

    if (!manual_wb(s, gr, gg, gb)) {
        fail = "Could not set manual white balance";
        goto done;
    }
    for (round = 1; round <= MAX_ROUNDS; round++) {
        char prog[sizeof s_msg];
        snprintf(prog, sizeof prog, "Matching %d of %d", round, MAX_ROUNDS);
        set_status(1, prog);
        if (!discard_frames(SKIP_FRAMES) || !average_mask(&mr, &mg, &mb)) {
            fail = "A frame was missed during matching";
            goto done;
        }
        matched = near(mr, tr, MATCH_TOL) && near(mg, tg, MATCH_TOL) && near(mb, tb, MATCH_TOL);
        double err = chan_gap(mr, tr);
        double eg = chan_gap(mg, tg);
        double eb = chan_gap(mb, tb);
        if (eg > err)
            err = eg;
        if (eb > err)
            err = eb;
        if (err < berr) {
            berr = err;
            br = gr;
            bg = gg;
            bb = gb;
            bmr = mr;
            bmg = mg;
            bmb = mb;
        }
        ESP_LOGI(TAG, "r%d PRESET R=0x%02X G=0x%02X B=0x%02X %s",
                 round, gr, gg, gb, matched ? "matched" : "NOT matched");
        ESP_LOGI(TAG, "r%d target %.0f/%.0f/%.0f manual %.0f/%.0f/%.0f",
                 round, tr, tg, tb, mr, mg, mb);
        if (matched)
            break;
        int nr = step_gain(gr, tr, mr);
        int ng = step_gain(gg, tg, mg);
        int nb = step_gain(gb, tb, mb);
        if (nr < 0 || ng < 0 || nb < 0) {
            fail = "Could not match the sky colour";
            goto done;
        }
        gr = nr;
        gg = ng;
        gb = nb;
        if (!manual_wb(s, gr, gg, gb)) {
            fail = "Could not set manual white balance";
            goto done;
        }
    }
    if (!matched && berr <= CHECK_TOL) {
        gr = br;
        gg = bg;
        gb = bb;
        mr = bmr;
        mg = bmg;
        mb = bmb;
        matched = true;
        ESP_LOGI(TAG, "closest R=0x%02X G=0x%02X B=0x%02X err %.0f", gr, gg, gb, berr);
    }
    if (!matched) {
        fail = "Could not match the sky colour";
        goto done;
    }

    set_status(1, "Checking the scene");
    if (!auto_wb(s) || !discard_frames(SETTLE_FRAMES) || !average_mask(&cr, &cg, &cb)) {
        fail = "Could not recheck automatic white balance";
        goto done;
    }
    ESP_LOGI(TAG, "r%d target %.0f/%.0f/%.0f manual %.0f/%.0f/%.0f check %.0f/%.0f/%.0f",
             round, tr, tg, tb, mr, mg, mb, cr, cg, cb);
    if (!near(cr, tr, CHECK_TOL) || !near(cg, tg, CHECK_TOL) || !near(cb, tb, CHECK_TOL)) {
        ESP_LOGW(TAG, "r%d scene or light changed during the round, discard", round);
        fail = "The light changed during calibration. Try again.";
        goto done;
    }

    unfreeze(s);
    xSemaphoreGive(g_cam_mu);
    held = false;

    why = hp10_sky_awb_preset_save(s_name, gr, gg, gb);
    if (why) {
        hp10_sky_awb_apply_locked();
        set_status(3, why);
        vTaskDelete(NULL);
        return;
    }
    hp10_cfg_save();
    snprintf(okmsg, sizeof okmsg, "Saved %s. R %d G %d B %d", s_name, gr, gg, gb);
    ESP_LOGI(TAG, "%s", okmsg);
    set_status(2, okmsg);
    vTaskDelete(NULL);
    return;

done:
    unfreeze(s);
    if (held)
        xSemaphoreGive(g_cam_mu);
    hp10_sky_awb_apply_locked();
    set_status(3, fail);
    vTaskDelete(NULL);
}

const char *hp10_sky_awb_cal_begin(const char *name)
{
    if (hp10_sky_awb_cal_busy())
        return "Calibration is running.";
    if (!g_bCameraOk)
        return "Camera is not ready";
    const char *why = hp10_sky_awb_name_ok(name);
    if (why)
        return why;
    bool full = true;
    for (int i = 0; i < SKY_AWB_PRESET_N; i++) {
        if (!g_sky_preset[i].used) {
            full = false;
            break;
        }
    }
    if (full)
        return "No more presets can be saved.";

    char stored[SKY_AWB_NAME_MAX + 1];
    snprintf(stored, sizeof stored, "%s", name);
    while (stored[0] == ' ')
        memmove(stored, stored + 1, strlen(stored));
    size_t n = strlen(stored);
    while (n > 0 && stored[n - 1] == ' ')
        stored[--n] = 0;

    portENTER_CRITICAL(&s_mux);
    memcpy(s_name, stored, sizeof s_name);
    s_state = 1;
    snprintf(s_msg, sizeof s_msg, "%s", "Settling automatic white balance");
    portEXIT_CRITICAL(&s_mux);

    if (xTaskCreate(cal_task, "awb_cal", 16384, NULL, 3, NULL) != pdPASS) {
        set_status(3, "Could not start calibration");
        return "Could not start calibration";
    }
    return NULL;
}
