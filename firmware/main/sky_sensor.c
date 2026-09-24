#include "esp_camera.h"
#include "esp_log.h"

#include <math.h>
#include <string.h>

#include "sky_cfg.h"
#include "sky_sensor.h"

static const char *TAG = "sky";

/* get_reg: high byte is the bank. 1 = sensor, 0 = DSP. */
#define REG_GAIN  0x100
#define REG_REG04 0x104
#define REG_AEC   0x110
#define REG_REG45 0x145
#define REG_AWB_R 0x0CC
#define REG_AWB_G 0x0CD
#define REG_AWB_B 0x0CE

static double gain_multiplier(int reg)
{
    double coarse = (double)(((reg >> 7) & 1) + 1)
                  * (double)(((reg >> 6) & 1) + 1)
                  * (double)(((reg >> 5) & 1) + 1)
                  * (double)(((reg >> 4) & 1) + 1);
    return coarse * (1.0 + (double)(reg & 0x0F) / 16.0);
}

static int read_reg(sensor_t *s, int reg, int mask)
{
    int v = s->get_reg(s, reg, mask);
    if (v < 0)
        return -1;
    return v & mask;
}

void sky_sensor_read(double luma, sky_sensor_t *out)
{
    memset(out, 0, sizeof *out);
    out->awb_mode = g_sky_awb;

    /* Caller holds g_cam_mu. Bank select is cached inside the driver. */
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->get_reg) {
        ESP_LOGW(TAG, "sensor read skipped");
        return;
    }

    int reg04 = read_reg(s, REG_REG04, 0x03);
    int aec_m = read_reg(s, REG_AEC, 0xFF);
    int reg45 = read_reg(s, REG_REG45, 0x3F);
    int gain = read_reg(s, REG_GAIN, 0xFF);
    if (reg04 < 0 || aec_m < 0 || reg45 < 0 || gain < 0) {
        ESP_LOGW(TAG, "sensor read failed");
        return;
    }

    out->read_ok = true;
    out->aec = (reg45 << 10) | (aec_m << 2) | reg04;
    out->gain_reg = gain;
    out->gain_x = gain_multiplier(gain);
    if (luma > 0.0 && out->aec > 0 && out->gain_x > 0.0) {
        out->light_ok = true;
        /* Relative only: log2(luma / (aec * gain_x)). */
        out->light_idx = log2(luma / ((double)out->aec * out->gain_x));
    }

    if (out->awb_mode == 0)
        return;

    int r = read_reg(s, REG_AWB_R, 0xFF);
    int g = read_reg(s, REG_AWB_G, 0xFF);
    int b = read_reg(s, REG_AWB_B, 0xFF);
    if (r < 0 || g < 0 || b < 0)
        return;
    out->gains_ok = true;
    out->awb_r = r;
    out->awb_g = g;
    out->awb_b = b;
}
