#include "esp_camera.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "cam_cfg.h"
#include "hp10_bringup.h"
#include "pins.h"

static const char *TAG = "cam";

static bool s_saved;
static bool s_init_psram;
static int s_init_fs = -1;
static uint8_t s_framesize = HP10_CAM_FRAMESIZE;
static int8_t s_brightness;
static int8_t s_contrast;
static int8_t s_saturation;
static uint8_t s_hmirror;
static uint8_t s_vflip;

static bool framesize_ok(int v)
{
    switch (v) {
    case FRAMESIZE_QQVGA:
    case FRAMESIZE_HQVGA:
    case FRAMESIZE_QVGA:
    case FRAMESIZE_CIF:
    case FRAMESIZE_VGA:
    case FRAMESIZE_SVGA:
    case FRAMESIZE_XGA:
    case FRAMESIZE_SXGA:
    case FRAMESIZE_UXGA:
        return true;
    default:
        return false;
    }
}

static bool level_ok(int v)
{
    return v >= -2 && v <= 2;
}

int hp10_cam_cfg_framesize(bool psram)
{
    int fs = s_saved ? (int)s_framesize : (int)HP10_CAM_FRAMESIZE;
    if (!psram && fs > FRAMESIZE_VGA)
        fs = FRAMESIZE_VGA;
    return fs;
}

bool hp10_cam_cfg_saved(void)
{
    return s_saved;
}

void hp10_cam_cfg_note_init(bool psram)
{
    s_init_psram = psram;
    s_init_fs = hp10_cam_cfg_framesize(psram);
}

bool hp10_cam_cfg_needs_reboot(void)
{
    if (s_init_fs < 0)
        return false;
    return hp10_cam_cfg_framesize(s_init_psram) != s_init_fs;
}

const char *hp10_cam_cfg_set(int framesize, int brightness, int contrast,
                             int saturation, int hmirror, int vflip)
{
    if (!framesize_ok(framesize))
        return "resolution is not a supported size";
    if (!level_ok(brightness))
        return "brightness must be -2 to 2";
    if (!level_ok(contrast))
        return "contrast must be -2 to 2";
    if (!level_ok(saturation))
        return "saturation must be -2 to 2";
    if (hmirror != 0 && hmirror != 1)
        return "h_mirror must be 0 or 1";
    if (vflip != 0 && vflip != 1)
        return "v_flip must be 0 or 1";

    s_framesize = (uint8_t)framesize;
    s_brightness = (int8_t)brightness;
    s_contrast = (int8_t)contrast;
    s_saturation = (int8_t)saturation;
    s_hmirror = (uint8_t)hmirror;
    s_vflip = (uint8_t)vflip;
    s_saved = true;
    return NULL;
}

void hp10_cam_cfg_get(int *framesize, int *brightness, int *contrast,
                      int *saturation, int *hmirror, int *vflip)
{
    if (framesize)
        *framesize = s_saved ? (int)s_framesize : (int)HP10_CAM_FRAMESIZE;
    if (brightness)
        *brightness = s_saved ? s_brightness : 0;
    if (contrast)
        *contrast = s_saved ? s_contrast : 0;
    if (saturation)
        *saturation = s_saved ? s_saturation : 0;
    if (hmirror)
        *hmirror = s_saved ? s_hmirror : 0;
    if (vflip)
        *vflip = s_saved ? s_vflip : 0;
}

static int set_level(sensor_t *s, int (*fn)(sensor_t *, int), int v, const char *name)
{
    if (!fn) {
        ESP_LOGW(TAG, "%s unsupported", name);
        return -1;
    }
    int rc = fn(s, v);
    if (rc != 0)
        ESP_LOGW(TAG, "%s %d failed (%d)", name, v, rc);
    return rc;
}

void hp10_cam_cfg_apply(void)
{
    if (!s_saved || !g_bCameraOk)
        return;
    sensor_t *s = esp_camera_sensor_get();
    if (!s)
        return;

    int fs = hp10_cam_cfg_framesize(s_init_psram);
    if ((int)s->status.framesize != fs && s->set_framesize) {
        if (s->set_framesize(s, (framesize_t)fs) != 0)
            ESP_LOGW(TAG, "framesize %d failed", fs);
    }
    /* Level 0 is already the sensor default. set_brightness(0) still rewrites
     * the OV2640 SDE block and makes the image too bright. */
    if (s_saturation)
        set_level(s, s->set_saturation, s_saturation, "saturation");
    if (s_contrast)
        set_level(s, s->set_contrast, s_contrast, "contrast");
    if (s_brightness)
        set_level(s, s->set_brightness, s_brightness, "brightness");
    set_level(s, s->set_hmirror, s_hmirror, "h_mirror");
    set_level(s, s->set_vflip, s_vflip, "v_flip");
    ESP_LOGI(TAG, "apply fs=%d bri=%d con=%d sat=%d hm=%d vf=%d",
             fs, s_brightness, s_contrast, s_saturation, s_hmirror, s_vflip);
}

void hp10_cam_cfg_apply_locked(void)
{
    if (g_cam_mu)
        xSemaphoreTake(g_cam_mu, portMAX_DELAY);
    hp10_cam_cfg_apply();
    if (g_cam_mu)
        xSemaphoreGive(g_cam_mu);
}

static bool load_level(nvs_handle_t h, const char *key, int8_t *dst)
{
    int8_t v = 0;
    if (nvs_get_i8(h, key, &v) != ESP_OK || !level_ok(v))
        return false;
    *dst = v;
    return true;
}

void hp10_cam_cfg_load(nvs_handle_t h)
{
    uint8_t set = 0;
    if (nvs_get_u8(h, "cam_set", &set) != ESP_OK || set != 1)
        return;

    uint8_t fs = 0, hm = 0, vf = 0;
    int8_t bri = 0, con = 0, sat = 0;
    if (nvs_get_u8(h, "cam_fs", &fs) != ESP_OK || !framesize_ok(fs))
        return;
    if (!load_level(h, "cam_bri", &bri) ||
        !load_level(h, "cam_con", &con) ||
        !load_level(h, "cam_sat", &sat))
        return;
    if (nvs_get_u8(h, "cam_hm", &hm) != ESP_OK || (hm != 0 && hm != 1))
        return;
    if (nvs_get_u8(h, "cam_vf", &vf) != ESP_OK || (vf != 0 && vf != 1))
        return;

    s_framesize = fs;
    s_brightness = bri;
    s_contrast = con;
    s_saturation = sat;
    s_hmirror = hm;
    s_vflip = vf;
    s_saved = true;
    ESP_LOGI(TAG, "nvs fs=%u bri=%d con=%d sat=%d hm=%u vf=%u",
             fs, bri, con, sat, hm, vf);
}

void hp10_cam_cfg_save(nvs_handle_t h)
{
    if (!s_saved)
        return;
    nvs_set_u8(h, "cam_set", 1);
    nvs_set_u8(h, "cam_fs", s_framesize);
    nvs_set_i8(h, "cam_bri", s_brightness);
    nvs_set_i8(h, "cam_con", s_contrast);
    nvs_set_i8(h, "cam_sat", s_saturation);
    nvs_set_u8(h, "cam_hm", s_hmirror);
    nvs_set_u8(h, "cam_vf", s_vflip);
}
