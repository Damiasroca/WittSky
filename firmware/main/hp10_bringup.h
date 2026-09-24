#pragma once

#include "esp_err.h"
#include "esp_netif.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HP10_UPLOAD_URL_MAX 128

extern uint8_t      g_abStaMac[6];
extern bool         g_bLoggedIn;
extern bool         g_bCameraOk;
extern char         g_szStaSsid[33];
extern char         g_szStaPwd[65];
extern esp_netif_t *g_pNetifSta;
extern void        *g_cam_mu;

extern bool    g_upload_en;
extern bool    g_ecowitt_en; /* stock rtpmedia POST; exclusive with g_upload_en */
extern char    g_upload_url[HP10_UPLOAD_URL_MAX];
extern uint8_t g_ost_interval; /* UI value: 0=off, 1=5 min, … */
extern char    g_ota_url[HP10_UPLOAD_URL_MAX];

extern bool    g_ws_log_en;
extern char    g_ws_log_url[HP10_UPLOAD_URL_MAX];
extern uint8_t g_ws_log_level; /* esp_log_level_t: 1=E … 5=V */

extern char     g_szApPwd[65];
extern bool     g_ap_auto;
extern bool     g_ap_on; /* saved preference; off applies only while STA has an IP */
extern char     g_mdns_host[32];
extern bool     g_wd_en;
extern uint8_t  g_wd_cam_n;
extern uint8_t  g_wd_up_n;
extern uint8_t  g_wd_cap_n;
extern uint16_t g_wd_cap_m;

struct cJSON;

esp_err_t board_gpio_init(void);
esp_err_t camera_bringup(void);
void      hp10_camera_boot(void);
esp_err_t hp10_wifi_ap_start(void);
int       hp10_wifi_sta_apply(const char *ssid, const char *pwd);
bool      hp10_sta_has_ip(void);

void      hp10_cfg_load(void);
void      hp10_cfg_save(void);
bool      hp10_upload_url_ok(const char *url);
void      hp10_upload_start(void);
esp_err_t hp10_upload_run_now(uint32_t wait_ms);

void      hp10_health_init(void);
void      hp10_health_add_json(struct cJSON *o);
void      hp10_upload_result_set(bool ok, const char *msg);
void      hp10_wd_note_camera_ok(void);
bool      hp10_wd_note_camera_fail(const char *detail);
void      hp10_wd_note_upload_ok(void);
bool      hp10_wd_note_upload_fail(const char *detail);
void      hp10_reboot_soon(void);
void      hp10_factory_reset(void);
bool      hp10_mdns_host_ok(const char *s);
void      hp10_mdns_apply(void);
void      hp10_wifi_ap_apply(void);
void      hp10_wifi_ap_stop(void);
void      hp10_wifi_ap_resume(void);
void      hp10_wifi_ap_set(bool on);
void      hp10_wifi_ap_add_json(struct cJSON *o);
void      hp10_ap_auto_start(void);
void      hp10_sta_restore_start(void);
const char *hp10_sta_link(void);
esp_err_t hp10_camera_retry(void);
esp_err_t hp10_camera_ensure(void);
esp_err_t hp10_camera_recover(void);
bool      hp10_ap_pwd_ok(const char *s);

bool      hp10_ws_log_url_ok(const char *url);
void      hp10_ws_log_start(void);
void      hp10_ws_log_apply(void);
void      hp10_ws_log_status(char *buf, size_t n);

bool      hp10_ota_url_ok(const char *url);
int       hp10_ota_check(void);
int       hp10_ota_start(void);
unsigned  hp10_ota_pct(void);
bool      hp10_ota_busy(void);
bool      hp10_ota_failed(void);
bool      hp10_ota_ok(void);
bool      hp10_ota_has_update(void);
const char *hp10_ota_msg(void);

esp_err_t hp10_ota_local_begin(size_t total);
esp_err_t hp10_ota_local_write(const void *data, size_t n);
esp_err_t hp10_ota_local_finish(void);
void      hp10_ota_local_abort(void);
void      hp10_ota_reboot_soon(void);

void      hp10_systime_start(void);
bool      hp10_sun_minutes(int *now_min, int *rise_min, int *set_min);
void      hp10_clock_strings(char *date, size_t date_n,
                             char *tz, size_t tz_n,
                             char *sunrise, size_t sr_n,
                             char *sunset, size_t ss_n);
void      hp10_location_get(double *lat, double *lon, int32_t *utc_off, bool *set);
bool      hp10_location_set(double lat, double lon);
bool      hp10_tz_set(const char *iana);
void      hp10_tz_get(char *iana, size_t n);
const char *hp10_tz_posix(const char *iana);
void      hp10_tz_names_json(struct cJSON *arr);

void           hp10_eco_init(void);
void           hp10_eco_status_json(struct cJSON *o);
struct cJSON  *hp10_eco_login(const char *account, const char *password);
struct cJSON  *hp10_eco_register(const char *name);
struct cJSON  *hp10_eco_logout(void);

#ifdef __cplusplus
}
#endif
