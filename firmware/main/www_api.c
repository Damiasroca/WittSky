#include "cJSON.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cam_cfg.h"
#include "hp10_bringup.h"
#include "pins.h"
#include "www_priv.h"

static const char *TAG = "www";

static void b64_decode(const char *in, char *out, size_t out_n);

static esp_err_t get_version(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    char buf[64];
    snprintf(buf, sizeof buf, "Version: %s", HP10_VERSION);
    cJSON_AddStringToObject(o, "version", buf);
    cJSON_AddNumberToObject(o, "newVersion", 0);
    return send_json(req, o);
}

static esp_err_t set_login_info(httpd_req_t *req)
{
    char *body = recv_body(req);
    cJSON *in = body ? cJSON_Parse(body) : NULL;
    free(body);
    char pwd[65] = {0};
    cJSON *it = in ? cJSON_GetObjectItem(in, "pwd") : NULL;
    if (it && cJSON_IsString(it) && it->valuestring)
        b64_decode(it->valuestring, pwd, sizeof pwd);
    cJSON_Delete(in);

    bool ok;
    if (g_szApPwd[0])
        ok = strcmp(pwd, g_szApPwd) == 0;
    else
        ok = true;

    cJSON *o = cJSON_CreateObject();
    if (!ok) {
        g_bLoggedIn = false;
        cJSON_AddStringToObject(o, "status", "0");
        cJSON_AddStringToObject(o, "msg", "Wrong password");
        return send_json(req, o);
    }
    g_bLoggedIn = true;
    cJSON_AddStringToObject(o, "status", "1");
    cJSON_AddStringToObject(o, "online", "1");
    cJSON_AddStringToObject(o, "msg", "success");
    return send_json(req, o);
}

static esp_err_t get_ws_settings(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    char buf[32];
    snprintf(buf, sizeof buf, "%02X:%02X:%02X:%02X:%02X:%02X",
             g_abStaMac[0], g_abStaMac[1], g_abStaMac[2],
             g_abStaMac[3], g_abStaMac[4], g_abStaMac[5]);
    cJSON_AddStringToObject(o, "sta_mac", buf);
    char date[32], tz[88], sr[8], ss[8];
    hp10_clock_strings(date, sizeof date, tz, sizeof tz, sr, sizeof sr, ss, sizeof ss);
    cJSON_AddStringToObject(o, "date", date);
    cJSON_AddStringToObject(o, "time_zone", tz);
    cJSON_AddStringToObject(o, "sunrise", sr);
    cJSON_AddStringToObject(o, "sunset", ss);
    double lat = 0, lon = 0;
    int32_t utc_off = 0;
    bool loc = false;
    hp10_location_get(&lat, &lon, &utc_off, &loc);
    if (loc) {
        cJSON_AddNumberToObject(o, "lat", lat);
        cJSON_AddNumberToObject(o, "lon", lon);
    } else {
        cJSON_AddNullToObject(o, "lat");
        cJSON_AddNullToObject(o, "lon");
    }
    char iana[40];
    hp10_tz_get(iana, sizeof iana);
    cJSON_AddStringToObject(o, "tz_iana", iana);
    cJSON_AddNumberToObject(o, "utc_hours", utc_off / 3600.0);
    cJSON_AddNumberToObject(o, "ost_interval", g_ost_interval);
    cJSON_AddNumberToObject(o, "upload_en", g_upload_en ? 1 : 0);
    cJSON_AddNumberToObject(o, "ecowitt_en", g_ecowitt_en ? 1 : 0);
    cJSON_AddStringToObject(o, "upload_url", g_upload_url);
    cJSON_AddNumberToObject(o, "ws_log_en", g_ws_log_en ? 1 : 0);
    cJSON_AddNumberToObject(o, "ws_log_level", g_ws_log_level);
    cJSON_AddStringToObject(o, "ws_log_url", g_ws_log_url);
    char wstat[96];
    hp10_ws_log_status(wstat, sizeof wstat);
    cJSON_AddStringToObject(o, "ws_log_status", wstat);
    const char *wpath = (g_ws_log_url[0] == '/') ? g_ws_log_url : HP10_WS_LOG_PATH;
    cJSON_AddStringToObject(o, "ws_log_path", wpath);
    cJSON_AddNumberToObject(o, "ws_log_port", HP10_WS_LOG_PORT);
    const char *note = "Upload disabled";
    if (g_upload_en && g_ecowitt_en)
        note = "Custom URL and Ecowitt cannot both be on";
    else if (g_ecowitt_en && g_ost_interval == 0)
        note = "Set an interval";
    else if (g_ecowitt_en && !hp10_sta_has_ip())
        note = "Waiting for router connection";
    else if (g_ecowitt_en)
        note = "Ecowitt.net";
    else if (g_upload_en && !hp10_upload_url_ok(g_upload_url))
        note = "Set an http(s) upload URL";
    else if (g_upload_en && g_ost_interval == 0)
        note = "Set an interval";
    else if (g_upload_en && !hp10_sta_has_ip())
        note = "Waiting for router connection";
    else if (g_upload_en)
        note = "";
    cJSON_AddStringToObject(o, "no_uploaded", note);
    return send_json(req, o);
}

static esp_err_t get_timezones(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "zones");
    hp10_tz_names_json(arr);
    return send_json(req, o);
}

static int json_num(cJSON *it)
{
    if (!it)
        return 0;
    if (cJSON_IsString(it) && it->valuestring)
        return atoi(it->valuestring);
    return it->valueint;
}

static double json_dbl(cJSON *it)
{
    if (!it)
        return 0;
    if (cJSON_IsString(it) && it->valuestring)
        return atof(it->valuestring);
    if (cJSON_IsNumber(it))
        return it->valuedouble;
    return 0;
}

static esp_err_t set_ws_settings(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    char *body = recv_body(req);
    cJSON *in = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!in)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);

    cJSON *it = cJSON_GetObjectItem(in, "ost_interval");
    if (it) {
        int v = json_num(it);
        if (v < 0)
            v = 0;
        if (v > 5)
            v = 5;
        g_ost_interval = (uint8_t)v;
    }
    it = cJSON_GetObjectItem(in, "upload_en");
    if (it)
        g_upload_en = json_num(it) != 0;
    it = cJSON_GetObjectItem(in, "ecowitt_en");
    if (it)
        g_ecowitt_en = json_num(it) != 0;
    it = cJSON_GetObjectItem(in, "upload_url");
    if (it && cJSON_IsString(it) && it->valuestring) {
        if (!it->valuestring[0]) {
            g_upload_url[0] = 0;
        } else if (hp10_upload_url_ok(it->valuestring)) {
            strlcpy(g_upload_url, it->valuestring, sizeof g_upload_url);
        } else {
            cJSON_Delete(in);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "status", "0");
            cJSON_AddStringToObject(o, "msg", "URL must be http(s) and not ecowitt.net");
            return send_json(req, o);
        }
    }
    if (g_upload_en && g_ecowitt_en) {
        cJSON_Delete(in);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "0");
        cJSON_AddStringToObject(o, "msg", "Custom URL and Ecowitt upload cannot both be enabled");
        return send_json(req, o);
    }
    if (g_upload_en && !hp10_upload_url_ok(g_upload_url))
        g_upload_en = false;

    bool log_changed = false;
    it = cJSON_GetObjectItem(in, "ws_log_en");
    if (it) {
        g_ws_log_en = json_num(it) != 0;
        log_changed = true;
    }
    it = cJSON_GetObjectItem(in, "ws_log_level");
    if (it) {
        int v = json_num(it);
        if (v < 1)
            v = 1;
        if (v > 5)
            v = 5;
        g_ws_log_level = (uint8_t)v;
        log_changed = true;
    }
    it = cJSON_GetObjectItem(in, "ws_log_url");
    if (it && cJSON_IsString(it) && it->valuestring) {
        if (!hp10_ws_log_url_ok(it->valuestring)) {
            cJSON_Delete(in);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "status", "0");
            cJSON_AddStringToObject(o, "msg", "WebSocket URL must be /path or ws(s)://host...");
            return send_json(req, o);
        }
        strlcpy(g_ws_log_url, it->valuestring, sizeof g_ws_log_url);
        log_changed = true;
    }

    cJSON *lat_it = cJSON_GetObjectItem(in, "lat");
    cJSON *lon_it = cJSON_GetObjectItem(in, "lon");
    cJSON *tz_it = cJSON_GetObjectItem(in, "tz_iana");
    if (tz_it && cJSON_IsString(tz_it) && tz_it->valuestring && tz_it->valuestring[0]) {
        if (!hp10_tz_set(tz_it->valuestring)) {
            cJSON_Delete(in);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "status", "0");
            cJSON_AddStringToObject(o, "msg", "Unknown timezone");
            return send_json(req, o);
        }
    }
    if (lat_it && lon_it && !cJSON_IsNull(lat_it) && !cJSON_IsNull(lon_it)) {
        double lat = json_dbl(lat_it);
        double lon = json_dbl(lon_it);
        if (!hp10_location_set(lat, lon)) {
            cJSON_Delete(in);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad location");
        }
    }

    cJSON_Delete(in);
    hp10_cfg_save();
    if (log_changed)
        hp10_ws_log_apply();
    ESP_LOGI(TAG, "upload settings eco=%d custom=%d ost=%u url='%s' ws_log=%d lvl=%u ws='%s'",
             g_ecowitt_en ? 1 : 0, g_upload_en ? 1 : 0, g_ost_interval,
             g_upload_url, g_ws_log_en ? 1 : 0, (unsigned)g_ws_log_level,
             g_ws_log_url);
    return httpd_resp_send(req, "200 OK", 6);
}

static esp_err_t send_ok_text(httpd_req_t *req)
{
    char *body = recv_body(req);
    free(body);
    return httpd_resp_send(req, "200 OK", 6);
}

static int b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* axjs.js baseCode() is standard base64. */
static void b64_decode(const char *in, char *out, size_t out_n)
{
    size_t o = 0;
    int val = 0, bits = -8;
    if (!in || !out || !out_n)
        return;
    out[0] = 0;
    for (; *in && *in != '=' && o + 1 < out_n; in++) {
        int d = b64_val((unsigned char)*in);
        if (d < 0)
            continue;
        val = (val << 6) + d;
        bits += 6;
        if (bits >= 0) {
            out[o++] = (char)((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    out[o] = 0;
}

static void ip_str(esp_ip4_addr_t a, char *buf, size_t n)
{
    snprintf(buf, n, IPSTR, IP2STR(&a));
}

static esp_err_t get_network_info(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ssid", g_szStaSsid);
    cJSON_AddStringToObject(o, "wifi_pwd", "");
    char buf[16] = "0.0.0.0";
    esp_netif_ip_info_t ip = {0};
    if (g_pNetifSta)
        esp_netif_get_ip_info(g_pNetifSta, &ip);
    ip_str(ip.ip, buf, sizeof buf);
    cJSON_AddStringToObject(o, "wifi_ip", buf);
    ip_str(ip.netmask, buf, sizeof buf);
    cJSON_AddStringToObject(o, "wifi_mask", buf);
    ip_str(ip.gw, buf, sizeof buf);
    cJSON_AddStringToObject(o, "wifi_gateway", buf);
    cJSON_AddStringToObject(o, "mdns_host", g_mdns_host);
    return send_json(req, o);
}

static esp_err_t set_network_info(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    char *body = recv_body(req);
    cJSON *in = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!in) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "4");
        return send_json(req, o);
    }

    char ssid[33] = {0};
    char pwd[65] = {0};
    cJSON *it = cJSON_GetObjectItem(in, "ssid");
    if (it && cJSON_IsString(it) && it->valuestring)
        strlcpy(ssid, it->valuestring, sizeof ssid);
    it = cJSON_GetObjectItem(in, "wifi_pwd");
    if (it && cJSON_IsString(it) && it->valuestring)
        b64_decode(it->valuestring, pwd, sizeof pwd);
    cJSON_Delete(in);

    ESP_LOGI(TAG, "join ssid='%s' pwd_len=%u", ssid, (unsigned)strlen(pwd));
    char status[2] = {'0' + (char)hp10_wifi_sta_apply(ssid, pwd), 0};
    if (status[0] < '0' || status[0] > '4')
        status[0] = '4';
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", status);
    return send_json(req, o);
}

static esp_err_t scan_ssid(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    wifi_scan_config_t sc = { .show_hidden = true };
    int tries = 0;
    while (esp_wifi_scan_start(&sc, true) != ESP_OK && tries++ < 16)
        vTaskDelay(pdMS_TO_TICKS(500));
    if (tries >= 16) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "error");
        cJSON_AddStringToObject(o, "msg", "Sorry, scanning WiFi AP timed out");
        return send_json(req, o);
    }

    uint16_t n = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&n));
    if (n > 16)
        n = 16;
    wifi_ap_record_t *aps = calloc(n ? n : 1, sizeof *aps);
    if (!aps) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "status", "error");
        cJSON_AddStringToObject(o, "msg", "Sorry, there was an error scanning");
        return send_json(req, o);
    }
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&n, aps));

    cJSON *list = cJSON_CreateArray();
    for (uint16_t i = 0; i < n; i++) {
        if (!aps[i].ssid[0])
            continue;
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "ssid", (char *)aps[i].ssid);
        cJSON_AddNumberToObject(it, "auth", aps[i].authmode);
        cJSON_AddNumberToObject(it, "rssi", aps[i].rssi);
        cJSON_AddItemToArray(list, it);
    }
    free(aps);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "0");
    cJSON_AddStringToObject(o, "msg", "scan_ok");
    cJSON_AddItemToObject(o, "list", list);
    return send_json(req, o);
}

static uint8_t clamp_u8(int v, int lo, int hi)
{
    if (v < lo)
        return (uint8_t)lo;
    if (v > hi)
        return (uint8_t)hi;
    return (uint8_t)v;
}

static esp_err_t get_device_info(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "upgrade", 0);
    cJSON_AddNumberToObject(o, "apAuto", g_ap_auto ? 1 : 0);
    cJSON_AddNumberToObject(o, "apOn", g_ap_on ? 1 : 0);
    cJSON_AddNumberToObject(o, "newVersion", hp10_ota_has_update() ? 1 : 0);
    cJSON_AddStringToObject(o, "curr_msg", hp10_ota_msg());
    cJSON_AddStringToObject(o, "ota_url", g_ota_url);
    cJSON_AddNumberToObject(o, "time", 15);
    cJSON_AddStringToObject(o, "APpwd", "");
    cJSON_AddNumberToObject(o, "ap_pwd_set", g_szApPwd[0] ? 1 : 0);
    cJSON_AddStringToObject(o, "mdns_host", g_mdns_host);
    cJSON_AddNumberToObject(o, "wd_en", g_wd_en ? 1 : 0);
    cJSON_AddNumberToObject(o, "wd_cam_n", g_wd_cam_n);
    cJSON_AddNumberToObject(o, "wd_up_n", g_wd_up_n);
    cJSON_AddNumberToObject(o, "wd_cap_n", g_wd_cap_n);
    cJSON_AddNumberToObject(o, "wd_cap_m", g_wd_cap_m);
    return send_json(req, o);
}

static esp_err_t set_device_info(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    char *body = recv_body(req);
    cJSON *in = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!in)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);

    cJSON *it = cJSON_GetObjectItem(in, "sysrestore");
    if (it && json_num(it)) {
        cJSON_Delete(in);
        hp10_factory_reset();
        return httpd_resp_send(req, "200 OK", 6);
    }
    it = cJSON_GetObjectItem(in, "sysreboot");
    if (it && json_num(it)) {
        cJSON_Delete(in);
        hp10_reboot_soon();
        return httpd_resp_send(req, "200 OK", 6);
    }
    it = cJSON_GetObjectItem(in, "APpwd");
    if (it && cJSON_IsString(it) && it->valuestring) {
        char pwd[65] = {0};
        b64_decode(it->valuestring, pwd, sizeof pwd);
        if (!hp10_ap_pwd_ok(pwd)) {
            cJSON_Delete(in);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad password");
        }
        strlcpy(g_szApPwd, pwd, sizeof g_szApPwd);
        hp10_cfg_save();
        cJSON_Delete(in);
        hp10_reboot_soon();
        return httpd_resp_send(req, "200 OK", 6);
    }
    it = cJSON_GetObjectItem(in, "mdns_host");
    if (it && cJSON_IsString(it) && it->valuestring) {
        if (!hp10_mdns_host_ok(it->valuestring)) {
            cJSON_Delete(in);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad hostname");
        }
        strlcpy(g_mdns_host, it->valuestring, sizeof g_mdns_host);
        hp10_cfg_save();
        hp10_mdns_apply();
    }
    it = cJSON_GetObjectItem(in, "ota_url");
    if (it && cJSON_IsString(it) && it->valuestring) {
        if (!it->valuestring[0]) {
            g_ota_url[0] = 0;
        } else if (hp10_ota_url_ok(it->valuestring)) {
            strlcpy(g_ota_url, it->valuestring, sizeof g_ota_url);
        } else {
            cJSON_Delete(in);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad ota url");
        }
    }
    it = cJSON_GetObjectItem(in, "apAuto");
    if (it)
        g_ap_auto = json_num(it) != 0;
    it = cJSON_GetObjectItem(in, "apOn");
    if (it)
        hp10_wifi_ap_set(json_num(it) != 0);
    it = cJSON_GetObjectItem(in, "wd_en");
    if (it)
        g_wd_en = json_num(it) != 0;
    it = cJSON_GetObjectItem(in, "wd_cam_n");
    if (it)
        g_wd_cam_n = clamp_u8(json_num(it), 1, 20);
    it = cJSON_GetObjectItem(in, "wd_up_n");
    if (it)
        g_wd_up_n = clamp_u8(json_num(it), 1, 20);
    it = cJSON_GetObjectItem(in, "wd_cap_n");
    if (it)
        g_wd_cap_n = clamp_u8(json_num(it), 1, 20);
    it = cJSON_GetObjectItem(in, "wd_cap_m");
    if (it) {
        int v = json_num(it);
        if (v < 1)
            v = 1;
        if (v > 1440)
            v = 1440;
        g_wd_cap_m = (uint16_t)v;
    }
    cJSON_Delete(in);
    hp10_cfg_save();
    return httpd_resp_send(req, "200 OK", 6);
}

static esp_err_t get_health(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    hp10_health_add_json(o);
    return send_json(req, o);
}

static esp_err_t upload_now_post(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    char *body = recv_body(req);
    free(body);
    esp_err_t err = hp10_upload_run_now(50000);
    cJSON *o = cJSON_CreateObject();
    if (err == ESP_OK) {
        cJSON_AddNumberToObject(o, "status", 1);
        cJSON_AddStringToObject(o, "msg", "Upload ok");
    } else if (err == ESP_ERR_TIMEOUT) {
        cJSON_AddNumberToObject(o, "status", 0);
        cJSON_AddStringToObject(o, "msg", "Upload timed out");
    } else if (err == ESP_ERR_INVALID_STATE) {
        cJSON_AddNumberToObject(o, "status", 0);
        cJSON_AddStringToObject(o, "msg", "Upload not ready (destination, WiFi, or camera)");
    } else {
        cJSON_AddNumberToObject(o, "status", 0);
        cJSON_AddStringToObject(o, "msg", "Upload failed");
    }
    return send_json(req, o);
}

static esp_err_t retry_camera_post(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    char *body = recv_body(req);
    free(body);
    esp_err_t err = hp10_camera_retry();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "status", err == ESP_OK ? 1 : 0);
    cJSON_AddStringToObject(o, "msg", err == ESP_OK ? "Camera ok" : "Camera still down");
    return send_json(req, o);
}

static esp_err_t send_upgrade_pct(httpd_req_t *req, unsigned pct)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "status", 1);
    cJSON_AddNumberToObject(o, "size", pct);
    return send_json(req, o);
}

static esp_err_t send_upgrade_err(httpd_req_t *req, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "status", 0);
    cJSON_AddStringToObject(o, "msg", msg);
    return send_json(req, o);
}

static esp_err_t upgrade_process(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    char *body = recv_body(req);
    cJSON *in = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!in)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);

    cJSON *it = cJSON_GetObjectItem(in, "upgrade");
    char act[16] = {0};
    if (it && cJSON_IsString(it) && it->valuestring)
        strlcpy(act, it->valuestring, sizeof act);
    cJSON_Delete(in);

    if (strcmp(act, "check") == 0) {
        int rc = hp10_ota_check();
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "is_new", rc == 1);
        cJSON_AddStringToObject(o, "msg", hp10_ota_msg());
        if (rc < 0)
            cJSON_AddNumberToObject(o, "err", 1);
        return send_json(req, o);
    }
    if (strcmp(act, "start") == 0) {
        int rc = hp10_ota_start();
        if (rc < 0)
            return send_upgrade_err(req, "Get Firmware failed");
        if (rc == 0)
            return send_upgrade_err(req, "upgrade task is going on...");
        return send_upgrade_pct(req, 0);
    }
    if (strcmp(act, "running") == 0 || strcmp(act, "progress") == 0) {
        if (hp10_ota_failed())
            return send_upgrade_err(req, "upgrade_error");
        if (hp10_ota_busy() || hp10_ota_ok())
            return send_upgrade_pct(req, hp10_ota_ok() ? 100 : hp10_ota_pct());
        return send_upgrade_err(req, "upgrade_error");
    }
    if (strcmp(act, "over") == 0 || strcmp(act, "reboot") == 0)
        return send_upgrade_pct(req, 100);
    return send_upgrade_err(req, "upgrade_error");
}

static esp_err_t upgrade_upload(httpd_req_t *req)
{
    if (!g_bLoggedIn)
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, NULL);
    int n = req->content_len;
    if (n <= 0)
        return send_upgrade_err(req, "Bad firmware size");
    if (hp10_ota_local_begin((size_t)n) != ESP_OK)
        return send_upgrade_err(req, hp10_ota_msg());

    uint8_t *buf = malloc(1024);
    if (!buf) {
        hp10_ota_local_abort();
        return send_upgrade_err(req, "No memory");
    }
    int left = n;
    while (left > 0) {
        int want = left > 1024 ? 1024 : left;
        int r = httpd_req_recv(req, (char *)buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (r <= 0) {
            free(buf);
            hp10_ota_local_abort();
            return send_upgrade_err(req, "Upload aborted");
        }
        if (hp10_ota_local_write(buf, (size_t)r) != ESP_OK) {
            free(buf);
            return send_upgrade_err(req, hp10_ota_msg());
        }
        left -= r;
    }
    free(buf);
    if (hp10_ota_local_finish() != ESP_OK)
        return send_upgrade_err(req, hp10_ota_msg());
    hp10_ota_reboot_soon();
    return send_upgrade_pct(req, 100);
}

static esp_err_t get_video_info(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    sensor_t *s = g_bCameraOk ? esp_camera_sensor_get() : NULL;
    if (!s) {
        int fs, bri, con, sat, hm, vf;
        hp10_cam_cfg_get(&fs, &bri, &con, &sat, &hm, &vf);
        cJSON_AddNumberToObject(o, "resolution", fs);
        cJSON_AddNumberToObject(o, "quality", HP10_CAM_JPEG_QUALITY);
        if (hp10_cam_cfg_saved()) {
            cJSON_AddNumberToObject(o, "brightness", bri);
            cJSON_AddNumberToObject(o, "contrast", con);
            cJSON_AddNumberToObject(o, "saturation", sat);
            cJSON_AddNumberToObject(o, "h_mirror", hm);
            cJSON_AddNumberToObject(o, "v_flip", vf);
        }
        return send_json(req, o);
    }
    cJSON_AddNumberToObject(o, "resolution", s->status.framesize);
    cJSON_AddNumberToObject(o, "quality", s->status.quality);
    cJSON_AddNumberToObject(o, "brightness", s->status.brightness);
    cJSON_AddNumberToObject(o, "contrast", s->status.contrast);
    cJSON_AddNumberToObject(o, "saturation", s->status.saturation);
    cJSON_AddNumberToObject(o, "sharpness", s->status.sharpness);
    cJSON_AddNumberToObject(o, "special_effect", s->status.special_effect);
    cJSON_AddNumberToObject(o, "wb_mode", s->status.wb_mode);
    cJSON_AddNumberToObject(o, "awb", s->status.awb);
    cJSON_AddNumberToObject(o, "awb_gain", s->status.awb_gain);
    cJSON_AddNumberToObject(o, "aec_sensor", s->status.aec);
    cJSON_AddNumberToObject(o, "aec_dsp", s->status.aec2);
    cJSON_AddNumberToObject(o, "ae_level", s->status.ae_level);
    cJSON_AddNumberToObject(o, "exposure", s->status.aec_value);
    cJSON_AddNumberToObject(o, "agc", s->status.agc);
    cJSON_AddNumberToObject(o, "agc_gain", s->status.agc_gain);
    cJSON_AddNumberToObject(o, "gain_ceiling", s->status.gainceiling);
    cJSON_AddNumberToObject(o, "bpc", s->status.bpc);
    cJSON_AddNumberToObject(o, "wpc", s->status.wpc);
    cJSON_AddNumberToObject(o, "raw_gma", s->status.raw_gma);
    cJSON_AddNumberToObject(o, "lens_correction", s->status.lenc);
    cJSON_AddNumberToObject(o, "h_mirror", s->status.hmirror);
    cJSON_AddNumberToObject(o, "v_flip", s->status.vflip);
    cJSON_AddNumberToObject(o, "dcw_downsize", s->status.dcw);
    cJSON_AddNumberToObject(o, "color_bar", s->status.colorbar);
    cJSON_AddNumberToObject(o, "scale", s->status.scale);
    cJSON_AddNumberToObject(o, "binning", s->status.binning);
    cJSON_AddNumberToObject(o, "denoise", s->status.denoise);
    return send_json(req, o);
}

static int apply_cam(cJSON *root, const char *key, sensor_t *s,
                     int (*fn)(sensor_t *, int))
{
    cJSON *it = cJSON_GetObjectItem(root, key);
    if (!it || !s || !fn)
        return 0;
    int v = cJSON_IsString(it) ? atoi(it->valuestring) : it->valueint;
    fn(s, v);
    return 1;
}

static esp_err_t set_video_info(httpd_req_t *req)
{
    char *body = recv_body(req);
    cJSON *o = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!o)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);

    sensor_t *s = g_bCameraOk ? esp_camera_sensor_get() : NULL;
    if (s) {
        apply_cam(o, "resolution", s, (int (*)(sensor_t *, int))s->set_framesize);
        apply_cam(o, "quality", s, s->set_quality);
        apply_cam(o, "contrast", s, s->set_contrast);
        apply_cam(o, "brightness", s, s->set_brightness);
        apply_cam(o, "saturation", s, s->set_saturation);
        apply_cam(o, "gain_ceiling", s, (int (*)(sensor_t *, int))s->set_gainceiling);
        apply_cam(o, "awb", s, s->set_whitebal);
        apply_cam(o, "agc", s, s->set_gain_ctrl);
        apply_cam(o, "aec_sensor", s, s->set_exposure_ctrl);
        apply_cam(o, "h_mirror", s, s->set_hmirror);
        apply_cam(o, "v_flip", s, s->set_vflip);
        apply_cam(o, "awb_gain", s, s->set_awb_gain);
        apply_cam(o, "agc_gain", s, s->set_agc_gain);
        apply_cam(o, "aec_dsp", s, s->set_aec2);
        apply_cam(o, "special_effect", s, s->set_special_effect);
        apply_cam(o, "wb_mode", s, s->set_wb_mode);
        apply_cam(o, "ae_level", s, s->set_ae_level);
        apply_cam(o, "lens_correction", s, s->set_lenc);
    }
    cJSON_Delete(o);
    return send_ok_text(req);
}

static bool json_int(cJSON *in, const char *key, int *out)
{
    cJSON *it = cJSON_GetObjectItem(in, key);
    if (!it)
        return false;
    if (cJSON_IsString(it)) {
        if (!it->valuestring)
            return false;
        *out = atoi(it->valuestring);
        return true;
    }
    if (!cJSON_IsNumber(it))
        return false;
    *out = it->valueint;
    return true;
}

static esp_err_t reject_video(httpd_req_t *req, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "status", 0);
    cJSON_AddStringToObject(o, "msg", msg);
    return send_json(req, o);
}

static esp_err_t set_video_cfg(httpd_req_t *req)
{
    char *body = recv_body(req);
    cJSON *in = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!in)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);

    int fs, bri, con, sat, hm, vf;
    bool ok = json_int(in, "resolution", &fs) &&
              json_int(in, "brightness", &bri) &&
              json_int(in, "contrast", &con) &&
              json_int(in, "saturation", &sat) &&
              json_int(in, "h_mirror", &hm) &&
              json_int(in, "v_flip", &vf);
    cJSON_Delete(in);
    if (!ok)
        return reject_video(req, "missing video setting");

    const char *why = hp10_cam_cfg_set(fs, bri, con, sat, hm, vf);
    if (why)
        return reject_video(req, why);
    if (g_bCameraOk)
        hp10_cam_cfg_apply_locked();
    hp10_cfg_save();

    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "status", 1);
    cJSON_AddStringToObject(o, "msg",
                            hp10_cam_cfg_needs_reboot()
                                ? "Saved. Reboot to apply the new resolution."
                                : "ok");
    return send_json(req, o);
}

static camera_fb_t *take_fb(void)
{
    if (!g_cam_mu)
        return NULL;
    if (!g_bCameraOk) {
        esp_err_t err = hp10_camera_ensure();
        if (err != ESP_OK) {
            if (err != ESP_ERR_INVALID_STATE)
                hp10_wd_note_camera_fail("camera_init");
            return NULL;
        }
    }
    xSemaphoreTake(g_cam_mu, portMAX_DELAY);
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
        hp10_wd_note_camera_ok();
        return fb;
    }
    xSemaphoreGive(g_cam_mu);
    esp_err_t rc = hp10_camera_recover();
    if (rc != ESP_OK) {
        if (rc != ESP_ERR_INVALID_STATE)
            hp10_wd_note_camera_fail("camera_grab");
        return NULL;
    }
    xSemaphoreTake(g_cam_mu, portMAX_DELAY);
    fb = esp_camera_fb_get();
    if (!fb) {
        xSemaphoreGive(g_cam_mu);
        hp10_wd_note_camera_fail("camera_grab");
        return NULL;
    }
    hp10_wd_note_camera_ok();
    return fb;
}

static esp_err_t capture_get(httpd_req_t *req)
{
    camera_fb_t *fb = take_fb();
    if (!fb)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture");
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t e = httpd_resp_send(req, (char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    xSemaphoreGive(g_cam_mu);
    return e;
}

static esp_err_t stream_get(httpd_req_t *req)
{
    static const char *bound = "123456789000000000000987654321";

    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=123456789000000000000987654321");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    for (;;) {
        camera_fb_t *fb = take_fb();
        if (!fb)
            break;
        char hdr[128];
        int n = snprintf(hdr, sizeof hdr,
                         "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                         bound, (unsigned)fb->len);
        if (httpd_resp_send_chunk(req, hdr, n) != ESP_OK ||
            httpd_resp_send_chunk(req, (char *)fb->buf, fb->len) != ESP_OK) {
            esp_camera_fb_return(fb);
            xSemaphoreGive(g_cam_mu);
            break;
        }
        esp_camera_fb_return(fb);
        xSemaphoreGive(g_cam_mu);
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_FAIL;
}


static void json_strcpy(cJSON *in, const char *key, char *dst, size_t n)
{
    cJSON *it = in ? cJSON_GetObjectItem(in, key) : NULL;
    dst[0] = 0;
    if (it && cJSON_IsString(it) && it->valuestring)
        strlcpy(dst, it->valuestring, n);
}

static esp_err_t get_ecowitt_account(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    cJSON *o = cJSON_CreateObject();
    hp10_eco_status_json(o);
    return send_json(req, o);
}

static esp_err_t set_ecowitt_account(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    char *body = recv_body(req);
    cJSON *in = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!in)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    char acct[81], pwd[65];
    json_strcpy(in, "account", acct, sizeof acct);
    json_strcpy(in, "password", pwd, sizeof pwd);
    cJSON_Delete(in);
    cJSON *o = hp10_eco_login(acct, pwd);
    if (!o)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    return send_json(req, o);
}

static esp_err_t ecowitt_register_post(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    char *body = recv_body(req);
    cJSON *in = body ? cJSON_Parse(body) : NULL;
    bool bad = body && body[0] && !in;
    free(body);
    if (bad)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
    char name[32];
    json_strcpy(in, "name", name, sizeof name);
    cJSON_Delete(in);
    cJSON *o = hp10_eco_register(name);
    if (!o)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    return send_json(req, o);
}

static esp_err_t ecowitt_logout_post(httpd_req_t *req)
{
    if (guest_json(req))
        return ESP_OK;
    free(recv_body(req));
    cJSON *o = hp10_eco_logout();
    if (!o)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    return send_json(req, o);
}

void www_api_register(httpd_handle_t http, httpd_handle_t stream)
{
    register_get(http, "/get_version", get_version);
    register_get(http, "/get_ws_settings", get_ws_settings);
    register_get(http, "/get_timezones", get_timezones);
    register_get(http, "/get_network_info", get_network_info);
    register_get(http, "/usr_scan_ssid_list", scan_ssid);
    register_get(http, "/get_device_info", get_device_info);
    register_get(http, "/get_video_info", get_video_info);
    register_get(http, "/get_health", get_health);
    register_get(http, "/capture", capture_get);
    register_post(http, "/set_login_info", set_login_info);
    register_post(http, "/set_ws_settings", set_ws_settings);
    register_post(http, "/set_network_info", set_network_info);
    register_post(http, "/set_device_info", set_device_info);
    register_post(http, "/set_video_info", set_video_info);
    register_post(http, "/set_video_cfg", set_video_cfg);
    register_post(http, "/upgrade_process", upgrade_process);
    register_post(http, "/upgrade_upload", upgrade_upload);
    register_post(http, "/upload_now", upload_now_post);
    register_post(http, "/retry_camera", retry_camera_post);
    register_get(http, "/get_ecowitt_account", get_ecowitt_account);
    register_post(http, "/set_ecowitt_account", set_ecowitt_account);
    register_post(http, "/ecowitt_register", ecowitt_register_post);
    register_post(http, "/ecowitt_logout", ecowitt_logout_post);
    register_get(stream, "/stream", stream_get);
}
