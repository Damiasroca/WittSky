/* Health snapshot, application watchdog, mDNS helpers. */

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>
#include <time.h>

#include "hp10_bringup.h"
#include "pins.h"
#include "sky_stats.h"

static const char *TAG = "health";

#define RTC_MAGIC 0x48313057u

typedef struct {
    uint32_t magic;
    char     cause[64];
    uint32_t stamp[8];
    uint8_t  n;
} rtc_wd_t;

static RTC_DATA_ATTR rtc_wd_t s_rtc;

static SemaphoreHandle_t    s_mu;
static esp_reset_reason_t   s_reset;
static uint8_t              s_cam_fail;
static uint8_t              s_up_fail;
static bool                 s_held;
static bool                 s_up_have;
static bool                 s_up_ok;
static int64_t              s_up_us;
static char                 s_up_msg[96];
static bool                 s_mdns;
static bool                 s_rebooting;

char     g_szApPwd[65];
bool     g_ap_auto;
bool     g_ap_on = true;
char     g_mdns_host[32] = "camera";
bool     g_wd_en = true;
uint8_t  g_wd_cam_n = 3;
uint8_t  g_wd_up_n = 3;
uint8_t  g_wd_cap_n = 3;
uint16_t g_wd_cap_m = 60;

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_UNKNOWN:    return "UNKNOWN";
    case ESP_RST_POWERON:    return "POWERON";
    case ESP_RST_EXT:        return "EXT";
    case ESP_RST_SW:         return "SW";
    case ESP_RST_PANIC:      return "PANIC";
    case ESP_RST_INT_WDT:    return "INT_WDT";
    case ESP_RST_TASK_WDT:   return "TASK_WDT";
    case ESP_RST_WDT:        return "WDT";
    case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:   return "BROWNOUT";
    case ESP_RST_SDIO:       return "SDIO";
    default:                 return "OTHER";
    }
}

static void rtc_clear(void)
{
    memset(&s_rtc, 0, sizeof s_rtc);
}

static uint32_t now_s(void)
{
    time_t t = 0;
    time(&t);
    if (t < 0)
        t = 0;
    return (uint32_t)t;
}

static uint8_t cap_fresh(uint32_t now)
{
    uint32_t win = (uint32_t)g_wd_cap_m * 60u;
    if (win == 0)
        win = 60;
    uint8_t n = 0;
    if (s_rtc.magic != RTC_MAGIC)
        return 0;
    for (uint8_t i = 0; i < s_rtc.n && i < 8; i++) {
        uint32_t s = s_rtc.stamp[i];
        if (now >= s && (now - s) < win)
            n++;
    }
    return n;
}

static bool cap_allows(void)
{
    uint8_t cap = g_wd_cap_n ? g_wd_cap_n : 3;
    uint8_t n = cap_fresh(now_s());
    return n < cap;
}

static void cap_record(void)
{
    uint32_t now = now_s();
    uint32_t win = (uint32_t)g_wd_cap_m * 60u;
    if (win == 0)
        win = 60;
    uint32_t keep[8];
    uint8_t kn = 0;
    if (s_rtc.magic == RTC_MAGIC) {
        for (uint8_t i = 0; i < s_rtc.n && i < 8; i++) {
            uint32_t s = s_rtc.stamp[i];
            if (now >= s && (now - s) < win && kn < 8)
                keep[kn++] = s;
        }
    }
    if (kn >= 8) {
        memmove(keep, keep + 1, 7 * sizeof keep[0]);
        kn = 7;
    }
    keep[kn++] = now;
    memset(&s_rtc.stamp, 0, sizeof s_rtc.stamp);
    memcpy(s_rtc.stamp, keep, kn * sizeof keep[0]);
    s_rtc.n = kn;
    s_rtc.magic = RTC_MAGIC;
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

void hp10_reboot_soon(void)
{
    if (s_rebooting)
        return;
    s_rebooting = true;
    xTaskCreate(reboot_task, "hp10_rb", 2048, NULL, 5, NULL);
}

static bool maybe_reboot(const char *detail)
{
    if (!g_wd_en || s_rebooting)
        return false;
    if (!cap_allows()) {
        s_held = true;
        ESP_LOGW(TAG, "watchdog hold: %s (cap %u / %u min)",
                 detail ? detail : "?", (unsigned)g_wd_cap_n,
                 (unsigned)g_wd_cap_m);
        return false;
    }
    s_held = false;
    cap_record();
    strlcpy(s_rtc.cause, detail ? detail : "watchdog", sizeof s_rtc.cause);
    s_rtc.magic = RTC_MAGIC;
    ESP_LOGW(TAG, "watchdog reboot: %s", s_rtc.cause);
    hp10_reboot_soon();
    return true;
}

void hp10_health_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    s_reset = esp_reset_reason();
    if (s_reset == ESP_RST_POWERON || s_reset == ESP_RST_BROWNOUT) {
        rtc_clear();
    } else if (s_rtc.magic != RTC_MAGIC) {
        rtc_clear();
    }
    ESP_LOGI(TAG, "reset=%s prev='%s'", reset_name(s_reset),
             s_rtc.magic == RTC_MAGIC && s_rtc.cause[0] ? s_rtc.cause : "");
}

void hp10_upload_result_set(bool ok, const char *msg)
{
    if (s_mu)
        xSemaphoreTake(s_mu, portMAX_DELAY);
    s_up_have = true;
    s_up_ok = ok;
    s_up_us = esp_timer_get_time();
    strlcpy(s_up_msg, msg ? msg : "", sizeof s_up_msg);
    if (s_mu)
        xSemaphoreGive(s_mu);
}

void hp10_wd_note_camera_ok(void)
{
    if (s_mu)
        xSemaphoreTake(s_mu, portMAX_DELAY);
    s_cam_fail = 0;
    if (s_mu)
        xSemaphoreGive(s_mu);
}

bool hp10_wd_note_camera_fail(const char *detail)
{
    bool rb = false;
    if (s_mu)
        xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_cam_fail < 255)
        s_cam_fail++;
    uint8_t n = g_wd_cam_n ? g_wd_cam_n : 3;
    ESP_LOGW(TAG, "camera fail %u/%u (%s)", (unsigned)s_cam_fail,
             (unsigned)n, detail ? detail : "");
    if (s_cam_fail >= n)
        rb = maybe_reboot(detail ? detail : "camera_init");
    if (s_mu)
        xSemaphoreGive(s_mu);
    return rb;
}

void hp10_wd_note_upload_ok(void)
{
    if (s_mu)
        xSemaphoreTake(s_mu, portMAX_DELAY);
    s_up_fail = 0;
    if (s_mu)
        xSemaphoreGive(s_mu);
}

bool hp10_wd_note_upload_fail(const char *detail)
{
    bool rb = false;
    if (s_mu)
        xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_up_fail < 255)
        s_up_fail++;
    uint8_t n = g_wd_up_n ? g_wd_up_n : 3;
    ESP_LOGW(TAG, "upload fail %u/%u (%s)", (unsigned)s_up_fail,
             (unsigned)n, detail ? detail : "");
    if (s_up_fail >= n)
        rb = maybe_reboot(detail ? detail : "upload");
    if (s_mu)
        xSemaphoreGive(s_mu);
    return rb;
}

void hp10_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open("hp10", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    rtc_clear();
    ESP_LOGW(TAG, "factory reset");
    hp10_reboot_soon();
}

bool hp10_mdns_host_ok(const char *s)
{
    if (!s)
        return false;
    size_t n = strlen(s);
    if (n < 1 || n > 31)
        return false;
    if (s[0] == '-' || s[n - 1] == '-')
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-')
            continue;
        return false;
    }
    return true;
}

bool hp10_ap_pwd_ok(const char *s)
{
    if (!s)
        return false;
    size_t n = strlen(s);
    if (n == 0)
        return true;
    if (n < 8 || n > 63)
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '\'')
            continue;
        return false;
    }
    return true;
}

void hp10_mdns_apply(void)
{
    if (!hp10_sta_has_ip())
        return;
    if (!hp10_mdns_host_ok(g_mdns_host))
        strlcpy(g_mdns_host, "camera", sizeof g_mdns_host);
    if (!s_mdns) {
        if (mdns_init() != ESP_OK) {
            ESP_LOGE(TAG, "mdns_init failed");
            return;
        }
        s_mdns = true;
    }
    mdns_hostname_set(g_mdns_host);
    mdns_instance_name_set("HP10");
    mdns_service_remove("_http", "_tcp");
    if (mdns_service_add("HP10", "_http", "_tcp", HP10_HTTP_PORT, NULL, 0) != ESP_OK)
        ESP_LOGW(TAG, "mdns service add failed");
    ESP_LOGI(TAG, "mdns http://%s.local", g_mdns_host);
}

static const char *rssi_label(int rssi)
{
    if (rssi >= -60)
        return "good";
    if (rssi >= -75)
        return "fair";
    return "weak";
}

void hp10_health_add_json(cJSON *o)
{
    if (!o)
        return;
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddNumberToObject(o, "rssi", ap.rssi);
        cJSON_AddStringToObject(o, "rssi_label", rssi_label(ap.rssi));
    } else {
        cJSON_AddNullToObject(o, "rssi");
        cJSON_AddStringToObject(o, "rssi_label", "—");
    }
    cJSON_AddNumberToObject(o, "uptime_s",
                            (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(o, "heap_free", (double)esp_get_free_heap_size());
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    bool ps = esp_psram_is_initialized();
    cJSON_AddNumberToObject(o, "psram_free",
                            ps ? (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) : 0);
    cJSON_AddNumberToObject(o, "psram_size",
                            ps ? (double)esp_psram_get_size() : 0);
#else
    cJSON_AddNumberToObject(o, "psram_free", 0);
    cJSON_AddNumberToObject(o, "psram_size", 0);
#endif
    cJSON_AddNumberToObject(o, "camera_ok", g_bCameraOk ? 1 : 0);
    cJSON_AddStringToObject(o, "ssid", g_szStaSsid);
    char ip[16] = "0.0.0.0";
    if (g_pNetifSta) {
        esp_netif_ip_info_t inf = {0};
        if (esp_netif_get_ip_info(g_pNetifSta, &inf) == ESP_OK)
            snprintf(ip, sizeof ip, IPSTR, IP2STR(&inf.ip));
    }
    cJSON_AddStringToObject(o, "sta_ip", ip);
    cJSON_AddStringToObject(o, "sta_link", hp10_sta_link());
    cJSON_AddStringToObject(o, "reboot_reason", reset_name(s_reset));
    cJSON_AddStringToObject(o, "prev_cause",
                            s_rtc.magic == RTC_MAGIC ? s_rtc.cause : "");
    if (s_up_have) {
        int64_t age = (esp_timer_get_time() - s_up_us) / 1000000;
        if (age < 0)
            age = 0;
        cJSON_AddNumberToObject(o, "last_upload_age_s", (double)age);
        cJSON_AddNumberToObject(o, "last_upload_ok", s_up_ok ? 1 : 0);
        cJSON_AddStringToObject(o, "last_upload_msg", s_up_msg);
    } else {
        cJSON_AddNumberToObject(o, "last_upload_age_s", -1);
        cJSON_AddNumberToObject(o, "last_upload_ok", 0);
        cJSON_AddStringToObject(o, "last_upload_msg", "");
    }
    cJSON_AddNumberToObject(o, "cam_fail", s_cam_fail);
    cJSON_AddNumberToObject(o, "up_fail", s_up_fail);
    cJSON_AddNumberToObject(o, "wd_en", g_wd_en ? 1 : 0);
    cJSON_AddNumberToObject(o, "wd_cam_n", g_wd_cam_n);
    cJSON_AddNumberToObject(o, "wd_up_n", g_wd_up_n);
    cJSON_AddNumberToObject(o, "wd_cap_n", g_wd_cap_n);
    cJSON_AddNumberToObject(o, "wd_cap_m", g_wd_cap_m);
    cJSON_AddNumberToObject(o, "wd_reboots", cap_fresh(now_s()));
    cJSON_AddNumberToObject(o, "wd_held_off", s_held ? 1 : 0);
    char mac[18];
    snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X",
             g_abStaMac[0], g_abStaMac[1], g_abStaMac[2],
             g_abStaMac[3], g_abStaMac[4], g_abStaMac[5]);
    cJSON_AddStringToObject(o, "sta_mac", mac);
    cJSON_AddStringToObject(o, "mdns_host", g_mdns_host);
    hp10_wifi_ap_add_json(o);
    char sky[HP10_SKY_JSON_MAX];
    if (hp10_sky_latest(sky, sizeof sky))
        cJSON_AddRawToObject(o, "sky", sky);
    else
        cJSON_AddNullToObject(o, "sky");
}
