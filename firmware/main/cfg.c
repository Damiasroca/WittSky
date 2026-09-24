#include "esp_log.h"
#include "nvs.h"

#include <string.h>

#include "cam_cfg.h"
#include "hp10_bringup.h"
#include "sky_cfg.h"
#include "upload_priv.h"

static const char *TAG = "upload";

bool    g_upload_en;
bool    g_ecowitt_en;
char    g_upload_url[HP10_UPLOAD_URL_MAX];
uint8_t g_ost_interval = 1;
char    g_ota_url[HP10_UPLOAD_URL_MAX];

bool hp10_upload_url_ok(const char *url)
{
    if (!url || !url[0])
        return false;
    if (strlen(url) >= HP10_UPLOAD_URL_MAX)
        return false;
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))
        return false;
    char lower[HP10_UPLOAD_URL_MAX];
    strlcpy(lower, url, sizeof lower);
    for (char *p = lower; *p; p++) {
        if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p - 'A' + 'a');
    }
    if (strstr(lower, "ecowitt.net"))
        return false;
    return true;
}

static void exclusive_fixup(void)
{
    if (g_ecowitt_en && g_upload_en)
        g_upload_en = false;
}

void hp10_cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open("hp10", NVS_READONLY, &h) != ESP_OK)
        return;
    uint8_t en = 0, eco = 0, ost = 1;
    nvs_get_u8(h, "up_en", &en);
    nvs_get_u8(h, "eco_en", &eco);
    nvs_get_u8(h, "ost", &ost);
    size_t n = sizeof g_upload_url;
    nvs_get_str(h, "up_url", g_upload_url, &n);
    n = sizeof g_szStaSsid;
    nvs_get_str(h, "ssid", g_szStaSsid, &n);
    n = sizeof g_szStaPwd;
    nvs_get_str(h, "pwd", g_szStaPwd, &n);
    n = sizeof g_ota_url;
    nvs_get_str(h, "ota_url", g_ota_url, &n);
    uint8_t wsen = 0, wslvl = 3;
    nvs_get_u8(h, "wsl_en", &wsen);
    nvs_get_u8(h, "wsl_lvl", &wslvl);
    n = sizeof g_ws_log_url;
    nvs_get_str(h, "wsl_url", g_ws_log_url, &n);
    n = sizeof g_szApPwd;
    nvs_get_str(h, "appwd", g_szApPwd, &n);
    n = sizeof g_mdns_host;
    nvs_get_str(h, "mdns_host", g_mdns_host, &n);
    uint8_t ap_auto = 0, ap_on = 1, wd_en = 1, cam_n = 3, up_n = 3, cap_n = 3;
    uint16_t cap_m = 60;
    nvs_get_u8(h, "ap_auto", &ap_auto);
    nvs_get_u8(h, "ap_on", &ap_on);
    nvs_get_u8(h, "wd_en", &wd_en);
    nvs_get_u8(h, "wd_cam_n", &cam_n);
    nvs_get_u8(h, "wd_up_n", &up_n);
    nvs_get_u8(h, "wd_cap_n", &cap_n);
    nvs_get_u16(h, "wd_cap_m", &cap_m);
    hp10_sky_cfg_load(h);
    hp10_cam_cfg_load(h);
    nvs_close(h);
    g_ap_auto = ap_auto != 0;
    g_ap_on = ap_on != 0;
    g_wd_en = wd_en != 0;
    g_wd_cam_n = cam_n ? cam_n : 3;
    g_wd_up_n = up_n ? up_n : 3;
    g_wd_cap_n = cap_n ? cap_n : 3;
    g_wd_cap_m = cap_m ? cap_m : 60;
    if (!hp10_mdns_host_ok(g_mdns_host))
        strlcpy(g_mdns_host, "camera", sizeof g_mdns_host);
    if (!hp10_ap_pwd_ok(g_szApPwd))
        g_szApPwd[0] = 0;
    g_ws_log_en = wsen != 0;
    g_ws_log_level = wslvl;
    if (g_ws_log_level < 1)
        g_ws_log_level = 1;
    if (g_ws_log_level > 5)
        g_ws_log_level = 5;
    if (!hp10_ws_log_url_ok(g_ws_log_url)) {
        g_ws_log_url[0] = 0;
        g_ws_log_en = false;
    }
    g_upload_en = en != 0;
    g_ecowitt_en = eco != 0;
    g_ost_interval = ost;
    if (!hp10_upload_url_ok(g_upload_url)) {
        g_upload_url[0] = 0;
        g_upload_en = false;
    }
    exclusive_fixup();
    if (g_ota_url[0] && !hp10_ota_url_ok(g_ota_url))
        g_ota_url[0] = 0;
    log_cfg("nvs load");
}

void hp10_cfg_save(void)
{
    exclusive_fixup();
    nvs_handle_t h;
    if (nvs_open("hp10", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs save open failed");
        return;
    }
    nvs_set_u8(h, "up_en", g_upload_en ? 1 : 0);
    nvs_set_u8(h, "eco_en", g_ecowitt_en ? 1 : 0);
    nvs_set_u8(h, "ost", g_ost_interval);
    nvs_set_str(h, "up_url", g_upload_url);
    nvs_set_str(h, "ssid", g_szStaSsid);
    nvs_set_str(h, "pwd", g_szStaPwd);
    nvs_set_str(h, "ota_url", g_ota_url);
    nvs_set_u8(h, "wsl_en", g_ws_log_en ? 1 : 0);
    nvs_set_u8(h, "wsl_lvl", g_ws_log_level);
    nvs_set_str(h, "wsl_url", g_ws_log_url);
    nvs_set_str(h, "appwd", g_szApPwd);
    nvs_set_str(h, "mdns_host", g_mdns_host);
    nvs_set_u8(h, "ap_auto", g_ap_auto ? 1 : 0);
    nvs_set_u8(h, "ap_on", g_ap_on ? 1 : 0);
    nvs_set_u8(h, "wd_en", g_wd_en ? 1 : 0);
    nvs_set_u8(h, "wd_cam_n", g_wd_cam_n);
    nvs_set_u8(h, "wd_up_n", g_wd_up_n);
    nvs_set_u8(h, "wd_cap_n", g_wd_cap_n);
    nvs_set_u16(h, "wd_cap_m", g_wd_cap_m);
    hp10_sky_cfg_save(h);
    hp10_cam_cfg_save(h);
    nvs_commit(h);
    nvs_close(h);
    log_cfg("nvs save");
}
