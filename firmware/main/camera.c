#include "esp_camera.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include "esp_psram.h"
#endif

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cam_cfg.h"
#include "hp10_bringup.h"
#include "pins.h"
#include "sky_awb.h"

static const char *TAG = "hp10";

esp_err_t board_gpio_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_CAM_POWER) |
                        (1ULL << PIN_STATUS_LED) |
                        (1ULL << PIN_GPIO12_EN) |
                        (1ULL << PIN_GPIO4_FLASH),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out);
    /* High = camera rail off. WiFi PHY calibration browns out if this
     * load is already on. camera_bringup drives it low. */
    gpio_set_level(PIN_CAM_POWER, 1);
    gpio_set_level(PIN_STATUS_LED, LED_ON_LEVEL);
    gpio_set_level(PIN_GPIO12_EN, 1);
    gpio_set_level(PIN_GPIO4_FLASH, 0);

    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << PIN_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&btn);
}

static void cam_pwdn_release(int end_level, int low_ms, int high_ms)
{
    gpio_set_level(PIN_CAM_PWDN, end_level ? 0 : 1);
    vTaskDelay(pdMS_TO_TICKS(low_ms));
    gpio_set_level(PIN_CAM_PWDN, end_level);
    vTaskDelay(pdMS_TO_TICKS(high_ms));
}

esp_err_t camera_bringup(void)
{
    bool psram = false;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    psram = esp_psram_is_initialized();
    ESP_LOGI(TAG, "psram %s size=%u", psram ? "ok" : "no",
             psram ? (unsigned)esp_psram_get_size() : 0);
#endif
    /* Stock drives GPIO 2 low, then inits with fb_count=1. */
    gpio_set_level(PIN_CAM_POWER, 0);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Own GPIO32 so modern esp32-camera cannot invert stock PWDN polarity.
     * FUN_400e5b68: level 0, vTaskDelay(20), level 1, vTaskDelay(50)
     * (200 ms / 500 ms at the stock 100 Hz tick). DZ0223 is OV2640. */
    gpio_config_t pwdn = {
        .pin_bit_mask = 1ULL << PIN_CAM_PWDN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwdn);

    camera_config_t cfg = {
        .pin_pwdn = -1,
        .pin_reset = PIN_CAM_RESET,
        .pin_xclk = PIN_CAM_XCLK,
        .pin_sccb_sda = PIN_CAM_SIOD,
        .pin_sccb_scl = PIN_CAM_SIOC,
        .pin_d7 = PIN_CAM_D7,
        .pin_d6 = PIN_CAM_D6,
        .pin_d5 = PIN_CAM_D5,
        .pin_d4 = PIN_CAM_D4,
        .pin_d3 = PIN_CAM_D3,
        .pin_d2 = PIN_CAM_D2,
        .pin_d1 = PIN_CAM_D1,
        .pin_d0 = PIN_CAM_D0,
        .pin_vsync = PIN_CAM_VSYNC,
        .pin_href = PIN_CAM_HREF,
        .pin_pclk = PIN_CAM_PCLK,
        .xclk_freq_hz = HP10_CAM_XCLK_HZ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = (framesize_t)hp10_cam_cfg_framesize(psram),
        .jpeg_quality = HP10_CAM_JPEG_QUALITY,
        .fb_count = 1,
        .fb_location = psram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };
    cam_pwdn_release(1, 200, 500);
    ESP_LOGI(TAG, "camera xclk=%d quality=%d fs=%d fb=%s pwr=%d pwdn=%d",
             HP10_CAM_XCLK_HZ, HP10_CAM_JPEG_QUALITY, (int)cfg.frame_size,
             cfg.fb_location == CAMERA_FB_IN_PSRAM ? "psram" : "dram",
             gpio_get_level(PIN_CAM_POWER), gpio_get_level(PIN_CAM_PWDN));
    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_camera_init %s (stock PWDN=1), try PWDN=0",
                 esp_err_to_name(err));
        esp_camera_deinit();
        gpio_reset_pin(PIN_CAM_XCLK);
        cam_pwdn_release(0, 10, 10);
        vTaskDelay(pdMS_TO_TICKS(50));
        err = esp_camera_init(&cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init: %s", esp_err_to_name(err));
        esp_camera_deinit();
        gpio_reset_pin(PIN_CAM_XCLK);
        gpio_set_level(PIN_STATUS_LED, LED_OFF_LEVEL);
        return err;
    }

    hp10_cam_cfg_note_init(psram);
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        ESP_LOGI(TAG, "sensor PID=0x%02x", s->id.PID);
        s->set_quality(s, HP10_CAM_JPEG_QUALITY);
    }
    g_bCameraOk = true;
    gpio_set_level(PIN_STATUS_LED, LED_ON_LEVEL);
    hp10_cam_cfg_apply();
    hp10_sky_awb_apply();
    return ESP_OK;
}

static int64_t s_cam_try_us;

static bool cam_recover_due(void)
{
    int64_t now = esp_timer_get_time();
    if (s_cam_try_us && (now - s_cam_try_us) < 8000000)
        return false;
    s_cam_try_us = now;
    return true;
}

esp_err_t hp10_camera_recover(void)
{
    if (!cam_recover_due())
        return ESP_ERR_INVALID_STATE;
    ESP_LOGW(TAG, "camera recover");
    return hp10_camera_retry();
}

esp_err_t hp10_camera_ensure(void)
{
    if (g_bCameraOk)
        return ESP_OK;
    return hp10_camera_recover();
}

esp_err_t hp10_camera_retry(void)
{
    if (g_cam_mu)
        xSemaphoreTake(g_cam_mu, portMAX_DELAY);
    if (g_bCameraOk) {
        esp_camera_deinit();
        g_bCameraOk = false;
    }
    int n = g_wd_cam_n ? g_wd_cam_n : 1;
    if (n > 5)
        n = 5;
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < n; i++) {
        err = camera_bringup();
        if (err == ESP_OK) {
            hp10_wd_note_camera_ok();
            if (g_cam_mu)
                xSemaphoreGive(g_cam_mu);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (g_cam_mu)
        xSemaphoreGive(g_cam_mu);
    return err;
}

void hp10_camera_boot(void)
{
    int n = 1;
    if (g_wd_en) {
        n = g_wd_cam_n ? g_wd_cam_n : 3;
        if (n > 8)
            n = 8;
    }
    for (int i = 0; i < n; i++) {
        if (i > 0)
            vTaskDelay(pdMS_TO_TICKS(250));
        /* SCCB bank select is a static in the sensor driver. */
        if (g_cam_mu)
            xSemaphoreTake(g_cam_mu, portMAX_DELAY);
        esp_err_t err = camera_bringup();
        if (g_cam_mu)
            xSemaphoreGive(g_cam_mu);
        if (err == ESP_OK) {
            hp10_wd_note_camera_ok();
            return;
        }
        if (g_wd_en && hp10_wd_note_camera_fail("camera_init"))
            return;
    }
    ESP_LOGW(TAG, "camera down — AP and web UI still start");
}
