/* SNTP clock. Sunrise / sunset / timezone come from the location
 * the user sets on Local Network — not from Ecowitt.
 */

#include "esp_log.h"
#include "esp_sntp.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "hp10_bringup.h"
#include "sun.h"

static const char *TAG = "systime";

static SemaphoreHandle_t s_mu;
static bool              s_sntp;
static bool              s_loc;
static double            s_lat;
static double            s_lon;
static int32_t           s_utc_off;
static uint8_t           s_sr_h, s_sr_m, s_ss_h, s_ss_m;
static bool              s_sun_ok;
static char              s_tz[80] = "Set timezone";
static char              s_tz_iana[40];
static char              s_tz_posix[64];
static int               s_isdst;

static void clock_lock(void)
{
    if (s_mu)
        xSemaphoreTake(s_mu, portMAX_DELAY);
}

static void clock_unlock(void)
{
    if (s_mu)
        xSemaphoreGive(s_mu);
}

static void apply_tz(void)
{
    if (!s_tz_posix[0])
        return;
    setenv("TZ", s_tz_posix, 1);
    tzset();
}

static void refresh_off(void)
{
    if (!s_tz_posix[0]) {
        s_isdst = 0;
        return;
    }
    time_t now = 0;
    time(&now);
    if (now < 1600000000)
        now = 1768867200; /* 2026-01-20 fallback until SNTP */
    struct tm loc = {0}, utc = {0};
    localtime_r(&now, &loc);
    gmtime_r(&now, &utc);
    int loc_sec = loc.tm_hour * 3600 + loc.tm_min * 60 + loc.tm_sec;
    int utc_sec = utc.tm_hour * 3600 + utc.tm_min * 60 + utc.tm_sec;
    int day = loc.tm_yday - utc.tm_yday;
    if (loc.tm_year != utc.tm_year)
        day = (loc.tm_year > utc.tm_year) ? 1 : -1;
    s_utc_off = (int32_t)(loc_sec - utc_sec + day * 86400);
    s_isdst = loc.tm_isdst > 0;
}

static void fmt_tz(char *out, size_t n)
{
    int32_t off = s_utc_off;
    int a = off < 0 ? -off : off;
    if (s_tz_iana[0]) {
        snprintf(out, n, "%s  (UTC%c%02d:%02d%s)",
                 s_tz_iana,
                 off < 0 ? '-' : '+', a / 3600, (a % 3600) / 60,
                 s_isdst ? " DST" : "");
        return;
    }
    if (off == 0) {
        strlcpy(out, "(UTC)", n);
        return;
    }
    snprintf(out, n, "(UTC%c%02d:%02d)",
             off < 0 ? '-' : '+', a / 3600, (a % 3600) / 60);
}

static void recompute_sun(void)
{
    refresh_off();
    if (!s_loc) {
        s_sun_ok = false;
        return;
    }

    time_t now = 0;
    time(&now);
    if (now < 1600000000)
        now = 1768867200;
    struct tm tm = {0};
    localtime_r(&now, &tm);

    int rise = 0, set = 0;
    bool ok = sun_rise_set(s_lat, s_lon, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                           s_utc_off, &rise, &set);
    s_sun_ok = ok;
    if (ok) {
        s_sr_h = (uint8_t)(rise / 60);
        s_sr_m = (uint8_t)(rise % 60);
        s_ss_h = (uint8_t)(set / 60);
        s_ss_m = (uint8_t)(set % 60);
    }
    fmt_tz(s_tz, sizeof s_tz);
}

static void persist(void)
{
    nvs_handle_t h;
    if (nvs_open("hp10", NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u8(h, "loc", s_loc ? 1 : 0);
    nvs_set_i32(h, "lat_e6", (int32_t)lround(s_lat * 1e6));
    nvs_set_i32(h, "lon_e6", (int32_t)lround(s_lon * 1e6));
    nvs_set_i32(h, "utc_off", s_utc_off);
    nvs_set_str(h, "tz_iana", s_tz_iana);
    nvs_commit(h);
    nvs_close(h);
}

static void load_saved(void)
{
    nvs_handle_t h;
    if (nvs_open("hp10", NVS_READONLY, &h) != ESP_OK)
        return;
    uint8_t loc = 0;
    int32_t lat_e6 = 0, lon_e6 = 0, off = 0;
    nvs_get_u8(h, "loc", &loc);
    nvs_get_i32(h, "lat_e6", &lat_e6);
    nvs_get_i32(h, "lon_e6", &lon_e6);
    nvs_get_i32(h, "utc_off", &off);
    size_t n = sizeof s_tz_iana;
    nvs_get_str(h, "tz_iana", s_tz_iana, &n);
    nvs_close(h);
    if (s_tz_iana[0]) {
        const char *p = hp10_tz_posix(s_tz_iana);
        if (p)
            strlcpy(s_tz_posix, p, sizeof s_tz_posix);
        else
            s_tz_iana[0] = 0;
    }
    apply_tz();
    if (!loc) {
        if (s_tz_iana[0]) {
            refresh_off();
            fmt_tz(s_tz, sizeof s_tz);
        }
        return;
    }
    s_loc = true;
    s_lat = lat_e6 / 1e6;
    s_lon = lon_e6 / 1e6;
    if (!s_tz_iana[0])
        s_utc_off = off;
    recompute_sun();
}

static void sntp_ensure(void)
{
    if (s_sntp)
        return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    if (CONFIG_LWIP_SNTP_MAX_SERVERS > 1)
        esp_sntp_setservername(1, "time.windows.com");
    if (CONFIG_LWIP_SNTP_MAX_SERVERS > 2)
        esp_sntp_setservername(2, "time.nist.gov");
    esp_sntp_init();
    s_sntp = true;
    ESP_LOGI(TAG, "SNTP started");
}

static void systime_task(void *arg)
{
    (void)arg;
    int last_day = -1;
    int unsync = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (!hp10_sta_has_ip()) {
            unsync = 0;
            continue;
        }
        sntp_ensure();
        time_t now = 0;
        time(&now);
        if (now < 1600000000) {
            /* SNTP keeps its own timer, but a join that raced the first
             * DNS lookup can sit unsynced until the next long retry. */
            if (++unsync >= 10) {
                unsync = 0;
                esp_sntp_restart();
                ESP_LOGW(TAG, "SNTP restart (clock still unset)");
            }
            continue;
        }
        unsync = 0;
        clock_lock();
        int prev_dst = s_isdst;
        refresh_off();
        time_t local_now = now;
        struct tm tm = {0};
        if (s_tz_posix[0])
            localtime_r(&local_now, &tm);
        else {
            local_now += s_utc_off;
            gmtime_r(&local_now, &tm);
        }
        if (s_loc && (tm.tm_yday != last_day || s_isdst != prev_dst)) {
            last_day = tm.tm_yday;
            recompute_sun();
            ESP_LOGI(TAG, "sun %s rise=%02u:%02u set=%02u:%02u",
                     s_tz, s_sr_h, s_sr_m, s_ss_h, s_ss_m);
        } else {
            fmt_tz(s_tz, sizeof s_tz);
        }
        clock_unlock();
    }
}

void hp10_location_get(double *lat, double *lon, int32_t *utc_off, bool *set)
{
    clock_lock();
    if (lat)
        *lat = s_lat;
    if (lon)
        *lon = s_lon;
    if (utc_off)
        *utc_off = s_utc_off;
    if (set)
        *set = s_loc;
    clock_unlock();
}

bool hp10_location_set(double lat, double lon)
{
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0)
        return false;
    clock_lock();
    s_loc = true;
    s_lat = lat;
    s_lon = lon;
    recompute_sun();
    persist();
    ESP_LOGI(TAG, "location lat=%.4f lon=%.4f %s rise=%02u:%02u set=%02u:%02u",
             lat, lon, s_tz, s_sr_h, s_sr_m, s_ss_h, s_ss_m);
    clock_unlock();
    return true;
}

bool hp10_tz_set(const char *iana)
{
    const char *p = hp10_tz_posix(iana);
    if (!p)
        return false;
    clock_lock();
    strlcpy(s_tz_iana, iana, sizeof s_tz_iana);
    strlcpy(s_tz_posix, p, sizeof s_tz_posix);
    apply_tz();
    recompute_sun();
    persist();
    ESP_LOGI(TAG, "timezone %s posix=%s off=%d dst=%d",
             s_tz_iana, s_tz_posix, (int)s_utc_off, s_isdst);
    clock_unlock();
    return true;
}

void hp10_tz_get(char *iana, size_t n)
{
    if (!iana || !n)
        return;
    clock_lock();
    strlcpy(iana, s_tz_iana, n);
    clock_unlock();
}

bool hp10_sun_minutes(int *now_min, int *rise_min, int *set_min)
{
    clock_lock();
    bool ok = s_loc && s_sun_ok;
    int rise = (int)s_sr_h * 60 + (int)s_sr_m;
    int set = (int)s_ss_h * 60 + (int)s_ss_m;
    bool use_tz = s_tz_posix[0] != 0;
    int32_t off = s_utc_off;
    clock_unlock();
    if (!ok)
        return false;

    time_t now = 0;
    time(&now);
    if (now < 1600000000)
        return false;
    struct tm tm = {0};
    if (use_tz)
        localtime_r(&now, &tm);
    else {
        now += off;
        gmtime_r(&now, &tm);
    }
    if (now_min)
        *now_min = tm.tm_hour * 60 + tm.tm_min;
    if (rise_min)
        *rise_min = rise;
    if (set_min)
        *set_min = set;
    return true;
}

void hp10_clock_strings(char *date, size_t date_n,
                        char *tz, size_t tz_n,
                        char *sunrise, size_t sr_n,
                        char *sunset, size_t ss_n)
{
    uint8_t sr_h, sr_m, ss_h, ss_m;
    bool loc, use_tz;
    int32_t off;
    clock_lock();
    loc = s_loc;
    use_tz = s_tz_posix[0] != 0;
    off = s_utc_off;
    sr_h = s_sr_h;
    sr_m = s_sr_m;
    ss_h = s_ss_h;
    ss_m = s_ss_m;
    if (tz && tz_n)
        strlcpy(tz, s_tz, tz_n);
    clock_unlock();

    if (date && date_n)
        date[0] = 0;
    time_t now = 0;
    time(&now);
    if (date && date_n && now > 1600000000) {
        struct tm tm = {0};
        if (use_tz) {
            localtime_r(&now, &tm);
        } else {
            now += off;
            gmtime_r(&now, &tm);
        }
        strftime(date, date_n, "%Y-%m-%d %H:%M:%S", &tm);
    }
    if (sunrise && sr_n) {
        if (loc)
            snprintf(sunrise, sr_n, "%02u:%02u", sr_h, sr_m);
        else
            strlcpy(sunrise, "00:00", sr_n);
    }
    if (sunset && ss_n) {
        if (loc)
            snprintf(sunset, ss_n, "%02u:%02u", ss_h, ss_m);
        else
            strlcpy(sunset, "00:00", ss_n);
    }
}

void hp10_systime_start(void)
{
    s_mu = xSemaphoreCreateMutex();
    load_saved();
    apply_tz();
    xTaskCreate(systime_task, "systime", 4096, NULL, 3, NULL);
}
