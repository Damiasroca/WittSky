#include "esp_camera.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "hp10_bringup.h"
#include "sky_awb.h"
#include "sky_cfg.h"

static const char *TAG = "sky";

void hp10_sky_awb_apply(void)
{
    if (!g_bCameraOk)
        return;
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->set_whitebal || !s->set_awb_gain || !s->set_wb_mode)
        return;

    if (s->set_whitebal(s, 1) != 0 || s->set_awb_gain(s, 1) != 0) {
        ESP_LOGW(TAG, "awb enable failed");
        return;
    }

    int mode = g_sky_awb;
    if (mode >= SKY_AWB_PRESET0 && mode <= SKY_AWB_PRESET_LAST) {
        sky_awb_preset_t *p = &g_sky_preset[mode - SKY_AWB_PRESET0];
        if (!p->used || !s->set_reg ||
            s->set_reg(s, 0x0C7, 0x40, 0x40) != 0 ||
            s->set_reg(s, 0x0CC, 0xFF, p->r) != 0 ||
            s->set_reg(s, 0x0CD, 0xFF, p->g) != 0 ||
            s->set_reg(s, 0x0CE, 0xFF, p->b) != 0) {
            ESP_LOGW(TAG, "awb preset %d failed", mode);
            return;
        }
        s->status.wb_mode = mode;
        ESP_LOGI(TAG, "awb preset %s %u %u %u",
                 p->name, (unsigned)p->r, (unsigned)p->g, (unsigned)p->b);
        return;
    }

    if (s->set_wb_mode(s, mode) != 0) {
        ESP_LOGW(TAG, "awb apply mode %d failed", mode);
        return;
    }
    ESP_LOGI(TAG, "awb mode %d", mode);
}

void hp10_sky_awb_apply_locked(void)
{
    if (g_cam_mu)
        xSemaphoreTake(g_cam_mu, portMAX_DELAY);
    hp10_sky_awb_apply();
    if (g_cam_mu)
        xSemaphoreGive(g_cam_mu);
}

bool hp10_sky_awb_applied(int *r, int *g, int *b)
{
    /* Same triples as the driver's wb_modes_regs. 0xCC/0xCD/0xCE are write latches. */
    static const uint8_t modes[4][3] = {
        { 0x5E, 0x41, 0x54 },
        { 0x65, 0x41, 0x4F },
        { 0x52, 0x41, 0x66 },
        { 0x42, 0x3F, 0x71 },
    };
    int mode = g_sky_awb;
    if (mode >= 1 && mode <= 4) {
        *r = modes[mode - 1][0];
        *g = modes[mode - 1][1];
        *b = modes[mode - 1][2];
        return true;
    }
    if (mode >= SKY_AWB_PRESET0 && mode <= SKY_AWB_PRESET_LAST) {
        sky_awb_preset_t *p = &g_sky_preset[mode - SKY_AWB_PRESET0];
        if (!p->used)
            return false;
        *r = p->r;
        *g = p->g;
        *b = p->b;
        return true;
    }
    return false;
}

const char *hp10_sky_awb_label(void)
{
    static const char *names[] = {
        "Auto", "Sunny", "Cloudy", "Office", "Home",
    };
    if (g_sky_awb <= 4)
        return names[g_sky_awb];
    int slot = g_sky_awb - SKY_AWB_PRESET0;
    if (slot >= 0 && slot < SKY_AWB_PRESET_N &&
        g_sky_preset[slot].used && g_sky_preset[slot].name[0])
        return g_sky_preset[slot].name;
    return "";
}
