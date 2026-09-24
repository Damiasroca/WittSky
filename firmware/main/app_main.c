/* HP10 bring-up: GPIO + camera + SoftAP + recovered web UI.
 * Not a port of the full V1.1.1 task graph.
 */

#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "hp10_bringup.h"
#include "www.h"

uint8_t      g_abStaMac[6];
bool         g_bLoggedIn;
bool         g_bCameraOk;
char         g_szStaSsid[33];
char         g_szStaPwd[65];
esp_netif_t *g_pNetifSta;
void        *g_cam_mu;

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    hp10_cfg_load();
    hp10_eco_init();
    hp10_health_init();
    g_cam_mu = xSemaphoreCreateMutex();

    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_ERROR_CHECK(board_gpio_init());
    /* PHY calibration is a sharp current spike. Run it before the
     * OV2640 is clocked, or the rail browns out and the chip resets. */
    ESP_ERROR_CHECK(hp10_wifi_ap_start());
    vTaskDelay(pdMS_TO_TICKS(300));
    hp10_camera_boot();
    www_start();
    hp10_systime_start();
    hp10_upload_start();
    hp10_ws_log_start();
    hp10_ap_auto_start();
    if (g_szStaSsid[0])
        hp10_sta_restore_start();
}
