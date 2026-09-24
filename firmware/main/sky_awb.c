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

    int mode = g_sky_awb;
    if (s->set_whitebal(s, 1) != 0 ||
        s->set_awb_gain(s, 1) != 0 ||
        s->set_wb_mode(s, mode) != 0) {
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
