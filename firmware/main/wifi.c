#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include "esp_mac.h"
#endif
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_ap_get_sta_list.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "hp10_bringup.h"
#include "pins.h"

static const char *TAG = "hp10";

#define EVT_GOT_IP   (1u << 0)
#define EVT_STA_DOWN (1u << 1)
#define EVT_LINK     (1u << 2)
#define EVT_NO_AP    (1u << 6)
#define EVT_BAD_PWD  (1u << 7)
#define EVT_OTHER    (1u << 8)

static EventGroupHandle_t s_wifi_evt;
static volatile bool      s_sta_joining;
static volatile int        s_next_ms;
static volatile TickType_t s_not_before;
static volatile bool       s_sta_bounce;
static const char         *s_link = "idle";

#define AP_LOG_N   12
#define AP_LOG_LEN 96

static SemaphoreHandle_t s_ap_mu;
static volatile bool     s_ap_radio = true;
static volatile TickType_t s_sta_since;
static portMUX_TYPE      s_ap_log_mux = portMUX_INITIALIZER_UNLOCKED;
static char              s_ap_log[AP_LOG_N][AP_LOG_LEN];
static int               s_ap_log_n;
static int               s_ap_log_pos;

static void fill_ap_cfg(wifi_config_t *ap);
static void sta_kick(void);
static void sta_link_task(void *arg);
static void ap_note(const char *msg);
static void ap_notef(const char *fmt, ...);
static void ap_mode_locked(bool on, const char *why);

static void ap_ssid(char *out, size_t n)
{
    snprintf(out, n, "HP10-WIFI%02X%02X", g_abStaMac[4], g_abStaMac[5]);
}

static void mac_str(const uint8_t *m, char *out, size_t n)
{
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void ap_note(const char *msg)
{
    char line[AP_LOG_LEN];
    int64_t s = esp_timer_get_time() / 1000000;
    int h = (int)(s / 3600);
    if (h > 9999)
        h = 9999;
    snprintf(line, sizeof line, "%d:%02d:%02d  %s",
             h, (int)((s / 60) % 60), (int)(s % 60), msg ? msg : "");
    portENTER_CRITICAL(&s_ap_log_mux);
    strlcpy(s_ap_log[s_ap_log_pos], line, AP_LOG_LEN);
    s_ap_log_pos = (s_ap_log_pos + 1) % AP_LOG_N;
    if (s_ap_log_n < AP_LOG_N)
        s_ap_log_n++;
    portEXIT_CRITICAL(&s_ap_log_mux);
    ESP_LOGI(TAG, "SoftAP %s", line);
}

static void ap_notef(const char *fmt, ...)
{
    char msg[72];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    ap_note(msg);
}

const char *hp10_sta_link(void)
{
    return s_link;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_evt, EVT_GOT_IP);
        xEventGroupClearBits(s_wifi_evt, EVT_NO_AP | EVT_BAD_PWD | EVT_OTHER);
        s_next_ms = 0;
        s_not_before = 0;
        s_link = "up";
        hp10_mdns_apply();
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_AP_STAIPASSIGNED && data) {
        ip_event_ap_staipassigned_t *e = data;
        char mac[18];
        mac_str(e->mac, mac, sizeof mac);
        ap_notef("leased " IPSTR " to %s", IP2STR(&e->ip), mac);
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(s_wifi_evt, EVT_GOT_IP);
        if (!s_sta_joining && g_szStaSsid[0]) {
            ESP_LOGW(TAG, "STA lost IP");
            s_link = "rejoining";
            sta_kick();
        }
        return;
    }
    if (base != WIFI_EVENT)
        return;
    if (id == WIFI_EVENT_AP_STACONNECTED && data) {
        wifi_event_ap_staconnected_t *e = data;
        char mac[18];
        mac_str(e->mac, mac, sizeof mac);
        ap_notef("client %s connected", mac);
        return;
    }
    if (id == WIFI_EVENT_AP_STADISCONNECTED && data) {
        wifi_event_ap_stadisconnected_t *e = data;
        char mac[18];
        mac_str(e->mac, mac, sizeof mac);
        ap_notef("client %s disconnected", mac);
        return;
    }
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        uint8_t reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        xEventGroupSetBits(s_wifi_evt, EVT_STA_DOWN);
        xEventGroupClearBits(s_wifi_evt, EVT_GOT_IP);
        if (!s_sta_joining) {
            if (s_sta_bounce || !g_szStaSsid[0])
                return;
            ESP_LOGW(TAG, "STA dropped reason=%u", reason);
            s_link = "rejoining";
            sta_kick();
            return;
        }
        ESP_LOGW(TAG, "STA disconnect reason=%u", reason);
        if (reason == WIFI_REASON_NO_AP_FOUND)
            xEventGroupSetBits(s_wifi_evt, EVT_NO_AP);
        else if (reason == WIFI_REASON_AUTH_EXPIRE ||
                 reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                 reason == WIFI_REASON_AUTH_FAIL ||
                 reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
                 reason == 200 || (reason >= 202 && reason <= 204))
            xEventGroupSetBits(s_wifi_evt, EVT_BAD_PWD);
        else
            xEventGroupSetBits(s_wifi_evt, EVT_OTHER);
    }
}

static void sta_kick(void)
{
    if (s_wifi_evt)
        xEventGroupSetBits(s_wifi_evt, EVT_LINK);
}

static int sta_apply_finish(int rc)
{
    s_sta_joining = false;
    if (rc != 0 && g_szStaSsid[0]) {
        /* A failed join leaves the driver on the new SSID. Put the saved one back. */
        s_next_ms = 0;
        s_not_before = 0;
        s_link = "rejoining";
        sta_kick();
    }
    return rc;
}

int hp10_wifi_sta_apply(const char *ssid, const char *pwd)
{
    if (!ssid || !ssid[0])
        return 4;

    s_sta_joining = true;
    s_link = "rejoining";
    uint32_t got = xEventGroupWaitBits(s_wifi_evt, EVT_GOT_IP, pdFALSE, pdTRUE, 0);
    if (got & EVT_GOT_IP) {
        xEventGroupClearBits(s_wifi_evt, EVT_GOT_IP);
        esp_wifi_disconnect();
        xEventGroupWaitBits(s_wifi_evt, EVT_STA_DOWN, pdFALSE, pdTRUE, pdMS_TO_TICKS(8000));
    }

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof cfg.sta.ssid);
    if (pwd)
        strlcpy((char *)cfg.sta.password, pwd, sizeof cfg.sta.password);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.failure_retry_cnt = 8;

    wifi_scan_config_t sc = {
        .ssid = (uint8_t *)ssid,
        .show_hidden = true,
    };
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 1;
        wifi_ap_record_t rec = {0};
        if (esp_wifi_scan_get_ap_records(&n, &rec) == ESP_OK && n && rec.ssid[0]) {
            memcpy(cfg.sta.bssid, rec.bssid, 6);
            cfg.sta.bssid_set = true;
            cfg.sta.channel = rec.primary;
            ESP_LOGI(TAG, "scan hit ch=%u rssi=%d auth=%u",
                     rec.primary, rec.rssi, rec.authmode);
        } else {
            ESP_LOGW(TAG, "targeted scan missed '%s'", ssid);
        }
    }

    xEventGroupClearBits(s_wifi_evt, EVT_GOT_IP | EVT_NO_AP | EVT_BAD_PWD | EVT_OTHER | EVT_STA_DOWN);
    s_sta_joining = true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        return sta_apply_finish(4);
    }

    int last = 3;
    for (int ticks = 120; ticks > 0; ticks--) {
        uint32_t bits = xEventGroupWaitBits(
            s_wifi_evt, EVT_GOT_IP | EVT_NO_AP | EVT_BAD_PWD | EVT_OTHER,
            pdFALSE, pdFALSE, 0);
        if (bits & EVT_GOT_IP) {
            strlcpy(g_szStaSsid, ssid, sizeof g_szStaSsid);
            strlcpy(g_szStaPwd, pwd ? pwd : "", sizeof g_szStaPwd);
            hp10_cfg_save();
            ESP_LOGI(TAG, "STA saved '%s'", g_szStaSsid);
            return sta_apply_finish(0);
        }
        if (bits & EVT_BAD_PWD)
            return sta_apply_finish(2);
        if (bits & EVT_NO_AP)
            last = 1;
        if (bits & EVT_OTHER)
            last = 3;
        if ((ticks % 8) == 0)
            esp_wifi_connect();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return sta_apply_finish(last);
}

esp_err_t hp10_wifi_ap_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_wifi_evt = xEventGroupCreate();
    esp_netif_create_default_wifi_ap();
    g_pNetifSta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    s_ap_mu = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    esp_read_mac(g_abStaMac, ESP_MAC_WIFI_STA);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event, NULL, NULL));

    wifi_config_t ap = {0};
    fill_ap_cfg(&ap);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    /* 0.25 dBm units. 60 = 15 dBm, matching CONFIG_ESP_PHY_MAX_WIFI_TX_POWER.
     * The PHY init spike uses that Kconfig value; this call covers later beacons. */
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(60));

    s_ap_radio = true;
    ap_notef("ON %s  http://192.168.4.1", ap.ap.ssid);
    if (!g_ap_on)
        ap_note("saved off, up until router link");
    xTaskCreate(sta_link_task, "sta_link", 4096, NULL, 4, NULL);
    return ESP_OK;
}

bool hp10_sta_has_ip(void)
{
    if (!g_pNetifSta)
        return false;
    esp_netif_ip_info_t ip = {0};
    if (esp_netif_get_ip_info(g_pNetifSta, &ip) != ESP_OK)
        return false;
    return ip.ip.addr != 0;
}

static void fill_ap_cfg(wifi_config_t *ap)
{
    memset(ap, 0, sizeof *ap);
    ap->ap.channel = HP10_AP_CHANNEL;
    ap->ap.max_connection = HP10_AP_MAX_CONN;
    ap_ssid((char *)ap->ap.ssid, sizeof ap->ap.ssid);
    ap->ap.ssid_len = (uint8_t)strlen((char *)ap->ap.ssid);
    if (g_szApPwd[0]) {
        strlcpy((char *)ap->ap.password, g_szApPwd, sizeof ap->ap.password);
        ap->ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap->ap.authmode = WIFI_AUTH_OPEN;
    }
}

void hp10_wifi_ap_apply(void)
{
    wifi_config_t ap = {0};
    fill_ap_cfg(&ap);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "AP config: %s", esp_err_to_name(err));
}

static void ap_mode_locked(bool on, const char *why)
{
    if (on == s_ap_radio)
        return;
    esp_err_t err;
    if (on) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err == ESP_OK)
            hp10_wifi_ap_apply();
    } else {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP %s: %s", on ? "on" : "off", esp_err_to_name(err));
        return;
    }
    s_ap_radio = on;
    ap_notef("%s (%s)", on ? "ON" : "OFF", why ? why : "");
}

void hp10_wifi_ap_stop(void)
{
    if (s_ap_mu)
        xSemaphoreTake(s_ap_mu, portMAX_DELAY);
    g_ap_on = false;
    if (hp10_sta_has_ip())
        ap_mode_locked(false, "saved off");
    else {
        ap_mode_locked(true, "STA down");
        ap_note("saved off, up until router link");
    }
    if (s_ap_mu)
        xSemaphoreGive(s_ap_mu);
}

void hp10_wifi_ap_resume(void)
{
    if (s_ap_mu)
        xSemaphoreTake(s_ap_mu, portMAX_DELAY);
    g_ap_on = true;
    s_sta_since = xTaskGetTickCount();
    ap_mode_locked(true, "manual");
    if (s_ap_mu)
        xSemaphoreGive(s_ap_mu);
}

void hp10_wifi_ap_set(bool on)
{
    if (on)
        hp10_wifi_ap_resume();
    else
        hp10_wifi_ap_stop();
}

void hp10_wifi_ap_add_json(cJSON *o)
{
    if (!o)
        return;
    bool on = s_ap_radio;
    char ssid[33];
    ap_ssid(ssid, sizeof ssid);
    cJSON_AddNumberToObject(o, "ap_on", on ? 1 : 0);
    cJSON_AddNumberToObject(o, "ap_hold", g_ap_on ? 1 : 0);
    cJSON_AddStringToObject(o, "ap_ssid", ssid);
    cJSON_AddNumberToObject(o, "ap_ch", HP10_AP_CHANNEL);

    char ip[16] = "";
    if (on) {
        esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        esp_netif_ip_info_t inf = {0};
        if (ap && esp_netif_get_ip_info(ap, &inf) == ESP_OK && inf.ip.addr)
            snprintf(ip, sizeof ip, IPSTR, IP2STR(&inf.ip));
        else
            strlcpy(ip, "192.168.4.1", sizeof ip);
    }
    cJSON_AddStringToObject(o, "ap_ip", ip);

    cJSON *leases = cJSON_CreateArray();
    int ncli = 0;
    if (on) {
        wifi_sta_list_t list = {0};
        if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) {
            ncli = list.num;
            if (ncli > ESP_WIFI_MAX_CONN_NUM)
                ncli = ESP_WIFI_MAX_CONN_NUM;
            wifi_sta_mac_ip_list_t ips = {0};
            bool have_ip = esp_wifi_ap_get_sta_list_with_ip(&list, &ips) == ESP_OK;
            for (int i = 0; i < ncli; i++) {
                cJSON *it = cJSON_CreateObject();
                char mac[18];
                mac_str(list.sta[i].mac, mac, sizeof mac);
                cJSON_AddStringToObject(it, "mac", mac);
                cJSON_AddNumberToObject(it, "rssi", list.sta[i].rssi);
                char ipb[16] = "";
                if (have_ip && ips.sta[i].ip.addr)
                    snprintf(ipb, sizeof ipb, IPSTR, IP2STR(&ips.sta[i].ip));
                cJSON_AddStringToObject(it, "ip", ipb);
                cJSON_AddItemToArray(leases, it);
            }
        }
    }
    cJSON_AddNumberToObject(o, "ap_clients", ncli);
    cJSON_AddItemToObject(o, "ap_leases", leases);

    char copy[AP_LOG_N][AP_LOG_LEN];
    int nlog = 0;
    int start = 0;
    portENTER_CRITICAL(&s_ap_log_mux);
    nlog = s_ap_log_n;
    start = (s_ap_log_n < AP_LOG_N) ? 0 : s_ap_log_pos;
    for (int i = 0; i < nlog; i++)
        memcpy(copy[i], s_ap_log[(start + i) % AP_LOG_N], AP_LOG_LEN);
    portEXIT_CRITICAL(&s_ap_log_mux);
    cJSON *log = cJSON_CreateArray();
    for (int i = 0; i < nlog; i++)
        cJSON_AddItemToArray(log, cJSON_CreateString(copy[i]));
    cJSON_AddItemToObject(o, "ap_log", log);
}

static void ap_auto_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_ap_mu)
            xSemaphoreTake(s_ap_mu, portMAX_DELAY);
        if (!hp10_sta_has_ip()) {
            s_sta_since = 0;
            ap_mode_locked(true, "STA down");
        } else if (!g_ap_on) {
            s_sta_since = 0;
            ap_mode_locked(false, "saved off");
        } else if (!g_ap_auto) {
            s_sta_since = 0;
            ap_mode_locked(true, "enabled");
        } else {
            if (!s_sta_since)
                s_sta_since = xTaskGetTickCount();
            if ((xTaskGetTickCount() - s_sta_since) >= pdMS_TO_TICKS(300000))
                ap_mode_locked(false, "auto");
        }
        if (s_ap_mu)
            xSemaphoreGive(s_ap_mu);
    }
}

void hp10_ap_auto_start(void)
{
    xTaskCreate(ap_auto_task, "ap_auto", 3072, NULL, 3, NULL);
}

static bool waiting_backoff(void)
{
    if (!s_not_before)
        return false;
    return (int32_t)(s_not_before - xTaskGetTickCount()) > 0;
}

static void sta_link_task(void *arg)
{
    (void)arg;
    for (;;) {
        xEventGroupWaitBits(s_wifi_evt, EVT_LINK, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
        if (s_sta_joining)
            continue;
        if (!g_szStaSsid[0]) {
            s_link = "idle";
            continue;
        }
        if (hp10_sta_has_ip()) {
            s_link = "up";
            s_next_ms = 0;
            s_not_before = 0;
            continue;
        }
        if (waiting_backoff()) {
            s_link = "backoff";
            continue;
        }

        s_link = "rejoining";
        ESP_LOGW(TAG, "STA rejoin '%s'", g_szStaSsid);

        wifi_ap_record_t assoc = {0};
        if (esp_wifi_sta_get_ap_info(&assoc) == ESP_OK) {
            xEventGroupClearBits(s_wifi_evt, EVT_STA_DOWN);
            s_sta_bounce = true;
            esp_wifi_disconnect();
            xEventGroupWaitBits(s_wifi_evt, EVT_STA_DOWN, pdTRUE, pdTRUE,
                                pdMS_TO_TICKS(4000));
            s_sta_bounce = false;
        }
        if (s_sta_joining || !g_szStaSsid[0])
            continue;

        wifi_config_t cfg = {0};
        strlcpy((char *)cfg.sta.ssid, g_szStaSsid, sizeof cfg.sta.ssid);
        strlcpy((char *)cfg.sta.password, g_szStaPwd, sizeof cfg.sta.password);
        cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        cfg.sta.failure_retry_cnt = 2;
        esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        if (cfg_err != ESP_OK)
            ESP_LOGW(TAG, "STA rejoin config: %s", esp_err_to_name(cfg_err));
        if (s_sta_joining)
            continue;
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN)
            ESP_LOGW(TAG, "STA rejoin connect: %s", esp_err_to_name(err));

        bool up = false;
        for (int i = 0; i < 60 && !s_sta_joining; i++) {
            uint32_t bits = xEventGroupWaitBits(
                s_wifi_evt, EVT_GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(500));
            if ((bits & EVT_GOT_IP) || hp10_sta_has_ip()) {
                up = true;
                break;
            }
        }
        if (s_sta_joining)
            continue;
        if (up) {
            s_link = "up";
            s_next_ms = 0;
            s_not_before = 0;
            ESP_LOGI(TAG, "STA rejoin ok");
            continue;
        }
        int delay = s_next_ms < 1000 ? 1000 : s_next_ms * 2;
        if (delay > 60000)
            delay = 60000;
        s_next_ms = delay;
        s_not_before = xTaskGetTickCount() + pdMS_TO_TICKS((TickType_t)delay);
        s_link = "backoff";
        ESP_LOGW(TAG, "STA rejoin failed, next in %d ms", delay);
    }
}

void hp10_sta_restore_start(void)
{
    if (g_szStaSsid[0])
        ESP_LOGI(TAG, "restore STA '%s'", g_szStaSsid);
    sta_kick();
}
