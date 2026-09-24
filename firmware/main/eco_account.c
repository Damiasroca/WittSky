/* Ecowitt.net account login and camera registration.
 *
 * Sign (same as the website): uppercase MD5 of the sorted
 * key=value pairs, encodeURIComponent with spaces as '+',
 * then "@ecowittnet" appended. That suffix is not sent.
 */

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "mbedtls/md.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "hp10_bringup.h"
#include "upload_priv.h"

static const char *TAG = "eco";

#define ECO_ACCT_MAX  80
#define ECO_PWD_MAX   64
#define ECO_NAME_MAX  31
#define ECO_UID_MAX   16
#define ECO_NICK_MAX  48
#define ECO_ID_MAX    16
#define ECO_WEB_VER   "v1.228_03_11"
#define ECO_UA        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36"

static char s_acct[ECO_ACCT_MAX + 1];
static char s_pwd[ECO_PWD_MAX + 1];
static char s_uid[ECO_UID_MAX + 1];
static char s_nick[ECO_NICK_MAX + 1];
static char s_devid[ECO_ID_MAX + 1];
static char s_check_err[128];
static int  s_checked; /* 1 after this boot's live device-list check */
static int  s_seen;    /* 1 when that list contained this camera's MAC */
static int  s_wait;

typedef struct {
    char name[24];
    char value[128];
} eco_ck_t;

static eco_ck_t s_cks[8];
static int      s_nck;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_done;
static TaskHandle_t      s_task;

enum {
    JOB_LOGIN = 1,
    JOB_REGISTER,
    JOB_LOGOUT,
};

static struct {
    int  job;
    char account[ECO_ACCT_MAX + 1];
    char password[ECO_PWD_MAX + 1];
    char name[ECO_NAME_MAX + 1];
    cJSON *out;
} s_job;

static char *s_body;

static int enc_uri(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        int plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (c == ' ') {
            if (n + 1 >= cap)
                return -1;
            out[n++] = '+';
        } else if (plain) {
            if (n + 1 >= cap)
                return -1;
            out[n++] = (char)c;
        } else {
            if (n + 3 >= cap)
                return -1;
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 0x0f];
        }
    }
    if (n >= cap)
        return -1;
    out[n] = 0;
    return (int)n;
}

static int md5_upper(const char *s, char out[33])
{
    uint8_t dig[16];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    if (!info || mbedtls_md(info, (const uint8_t *)s, strlen(s), dig) != 0)
        return -1;
    for (int i = 0; i < 16; i++)
        sprintf(out + 2 * i, "%02X", dig[i]);
    out[32] = 0;
    return 0;
}

typedef struct {
    const char *k;
    const char *v;
} eco_kv_t;

static int part_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int eco_sign(const eco_kv_t *items, int n, char out[33])
{
    char store[16][192];
    char *parts[16];
    char raw[1280];
    if (n <= 0 || n > 16)
        return -1;
    for (int i = 0; i < n; i++) {
        char enc[160];
        if (enc_uri(items[i].v, enc, sizeof enc) < 0)
            return -1;
        if (snprintf(store[i], sizeof store[i], "%s=%s", items[i].k, enc) >= (int)sizeof store[i])
            return -1;
        parts[i] = store[i];
    }
    qsort(parts, (size_t)n, sizeof parts[0], part_cmp);
    size_t len = 0;
    raw[0] = 0;
    for (int i = 0; i < n; i++) {
        size_t L = strlen(parts[i]);
        if (len + (i ? 1 : 0) + L + 12 >= sizeof raw)
            return -1;
        if (i)
            raw[len++] = '&';
        memcpy(raw + len, parts[i], L);
        len += L;
    }
    memcpy(raw + len, "@ecowittnet", 11);
    len += 11;
    raw[len] = 0;
    return md5_upper(raw, out);
}

static void ck_clear(void)
{
    s_nck = 0;
}

static void ck_put(const char *set_cookie)
{
    char pair[160];
    size_t i = 0;
    if (!set_cookie)
        return;
    while (set_cookie[i] && set_cookie[i] != ';' && i + 1 < sizeof pair) {
        pair[i] = set_cookie[i];
        i++;
    }
    pair[i] = 0;
    char *eq = strchr(pair, '=');
    if (!eq || eq == pair)
        return;
    *eq = 0;
    if (strlen(pair) >= sizeof s_cks[0].name || strlen(eq + 1) >= sizeof s_cks[0].value)
        return;
    for (int c = 0; c < s_nck; c++) {
        if (strcmp(s_cks[c].name, pair) == 0) {
            strlcpy(s_cks[c].value, eq + 1, sizeof s_cks[c].value);
            return;
        }
    }
    if (s_nck >= (int)(sizeof s_cks / sizeof s_cks[0]))
        return;
    strlcpy(s_cks[s_nck].name, pair, sizeof s_cks[s_nck].name);
    strlcpy(s_cks[s_nck].value, eq + 1, sizeof s_cks[s_nck].value);
    s_nck++;
}

static void ck_header(char *out, size_t n)
{
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < s_nck; i++) {
        int w = snprintf(out + len, n - len, "%s%s=%s",
                         len ? "; " : "", s_cks[i].name, s_cks[i].value);
        if (w < 0 || (size_t)w >= n - len) {
            out[0] = 0;
            return;
        }
        len += (size_t)w;
    }
}

typedef struct {
    size_t n;
    size_t cap;
    bool   trunc;
} eco_acc_t;

static esp_err_t eco_evt(esp_http_client_event_t *evt)
{
    eco_acc_t *a = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (evt->header_key && evt->header_value &&
            strcasecmp(evt->header_key, "Set-Cookie") == 0)
            ck_put(evt->header_value);
    } else if (evt->event_id == HTTP_EVENT_ON_DATA && a && s_body && evt->data_len > 0) {
        size_t room = a->cap - 1 - a->n;
        size_t take = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
        if ((size_t)evt->data_len > room)
            a->trunc = true;
        if (take) {
            memcpy(s_body + a->n, evt->data, take);
            a->n += take;
            s_body[a->n] = 0;
        }
    }
    return ESP_OK;
}

static void our_mac(char *out, size_t n)
{
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X",
             g_abStaMac[0], g_abStaMac[1], g_abStaMac[2],
             g_abStaMac[3], g_abStaMac[4], g_abStaMac[5]);
}

static bool mac_eq(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    for (;;) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'a' && ca <= 'z')
            ca = (unsigned char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z')
            cb = (unsigned char)(cb - 'a' + 'A');
        if (ca != cb)
            return false;
        if (!ca)
            return true;
    }
}

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open("hp10", NVS_READONLY, &h) != ESP_OK)
        return;
    size_t n = sizeof s_acct;
    nvs_get_str(h, "eco_acct", s_acct, &n);
    n = sizeof s_pwd;
    nvs_get_str(h, "eco_pwd", s_pwd, &n);
    n = sizeof s_uid;
    nvs_get_str(h, "eco_uid", s_uid, &n);
    n = sizeof s_nick;
    nvs_get_str(h, "eco_nick", s_nick, &n);
    n = sizeof s_devid;
    nvs_get_str(h, "eco_devid", s_devid, &n);
    nvs_close(h);
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open("hp10", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed");
        return;
    }
    nvs_set_str(h, "eco_acct", s_acct);
    nvs_set_str(h, "eco_pwd", s_pwd);
    nvs_set_str(h, "eco_uid", s_uid);
    nvs_set_str(h, "eco_nick", s_nick);
    nvs_set_str(h, "eco_devid", s_devid);
    nvs_commit(h);
    nvs_close(h);
}

static void add_id(cJSON *it, char *dst, size_t n)
{
    dst[0] = 0;
    if (!it)
        return;
    if (cJSON_IsString(it) && it->valuestring)
        strlcpy(dst, it->valuestring, n);
    else if (cJSON_IsNumber(it))
        snprintf(dst, n, "%d", it->valueint);
}

static void api_msg(cJSON *o, char *dst, size_t n, const char *fallback)
{
    cJSON *m = o ? cJSON_GetObjectItem(o, "errmsg") : NULL;
    if (!m || !cJSON_IsString(m))
        m = o ? cJSON_GetObjectItem(o, "msg") : NULL;
    if (m && cJSON_IsString(m) && m->valuestring && m->valuestring[0])
        strlcpy(dst, m->valuestring, n);
    else
        strlcpy(dst, fallback, n);
}

static int api_code(cJSON *o)
{
    cJSON *c = cJSON_GetObjectItem(o, "code");
    cJSON *e = cJSON_GetObjectItem(o, "errcode");
    if (c && cJSON_IsNumber(c))
        return c->valueint;
    if (e && cJSON_IsString(e) && e->valuestring)
        return atoi(e->valuestring);
    if (e && cJSON_IsNumber(e))
        return e->valueint;
    return -1;
}

static cJSON *http_json(esp_http_client_method_t method, const char *url,
                        const char *content_type, const char *referer,
                        const char *body, bool api, char *err, size_t err_n)
{
    eco_acc_t acc = { .cap = 16384 };
    char cookies[1200];
    if (!s_body) {
        strlcpy(err, "Out of memory", err_n);
        return NULL;
    }
    s_body[0] = 0;
    ck_header(cookies, sizeof cookies);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = 20000,
        .event_handler = eco_evt,
        .user_data = &acc,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        strlcpy(err, "HTTP client failed", err_n);
        return NULL;
    }
    esp_http_client_set_header(cli, "Accept", "application/json, text/plain, */*");
    esp_http_client_set_header(cli, "User-Agent", ECO_UA);
    esp_http_client_set_header(cli, "Origin", "https://www.ecowitt.net");
    if (referer)
        esp_http_client_set_header(cli, "Referer", referer);
    if (content_type)
        esp_http_client_set_header(cli, "Content-Type", content_type);
    if (api) {
        esp_http_client_set_header(cli, "accept-ecowittlang", "en");
        esp_http_client_set_header(cli, "web-version", ECO_WEB_VER);
    }
    if (cookies[0])
        esp_http_client_set_header(cli, "Cookie", cookies);
    if (body)
        esp_http_client_set_post_field(cli, body, (int)strlen(body));
    esp_err_t rc = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    if (rc != ESP_OK) {
        snprintf(err, err_n, "Ecowitt unreachable (%s)", esp_err_to_name(rc));
        return NULL;
    }
    if (status < 200 || status > 299) {
        snprintf(err, err_n, "Ecowitt HTTP %d", status);
        return NULL;
    }
    if (acc.trunc) {
        strlcpy(err, "Ecowitt response too large", err_n);
        return NULL;
    }
    cJSON *o = cJSON_Parse(s_body);
    if (!o) {
        strlcpy(err, "Bad response from Ecowitt", err_n);
        return NULL;
    }
    return o;
}

static int reg_now(void)
{
    return (s_checked && s_seen && s_devid[0]) ? 1 : 0;
}

static void fill_status(cJSON *o)
{
    char mac[18];
    our_mac(mac, sizeof mac);
    cJSON_AddStringToObject(o, "account", s_acct);
    cJSON_AddNumberToObject(o, "pwd_set", s_pwd[0] ? 1 : 0);
    cJSON_AddStringToObject(o, "uid", s_uid);
    cJSON_AddStringToObject(o, "nickname", s_nick);
    cJSON_AddNumberToObject(o, "logged_in", (s_nck > 0 && s_uid[0]) ? 1 : 0);
    cJSON_AddStringToObject(o, "device_id", reg_now() ? s_devid : "");
    cJSON_AddNumberToObject(o, "registered", reg_now());
    cJSON_AddNumberToObject(o, "checked", s_checked ? 1 : 0);
    cJSON_AddStringToObject(o, "check_error", s_check_err);
    cJSON_AddStringToObject(o, "mac", mac);
    cJSON_AddStringToObject(o, "suggest_name", g_mdns_host[0] ? g_mdns_host : "HP10");
}

static void put_str(cJSON *o, const char *k, const char *v)
{
    cJSON_DeleteItemFromObject(o, k);
    cJSON_AddStringToObject(o, k, v ? v : "");
}

static void put_num(cJSON *o, const char *k, int v)
{
    cJSON_DeleteItemFromObject(o, k);
    cJSON_AddNumberToObject(o, k, v);
}

static void refresh_live(cJSON *o)
{
    put_str(o, "uid", s_uid);
    put_str(o, "nickname", s_nick);
    put_str(o, "device_id", reg_now() ? s_devid : "");
    put_num(o, "registered", reg_now());
    put_num(o, "checked", s_checked ? 1 : 0);
    put_str(o, "check_error", s_check_err);
    put_num(o, "logged_in", (s_nck > 0 && s_uid[0]) ? 1 : 0);
}

static cJSON *result(const char *status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    cJSON_AddStringToObject(o, "status", status);
    cJSON_AddStringToObject(o, "msg", msg ? msg : "");
    fill_status(o);
    return o;
}

static bool net_ready(char *err, size_t n)
{
    if (!hp10_sta_has_ip()) {
        strlcpy(err, "Connect the camera to the router first", n);
        return false;
    }
    time_t now = 0;
    time(&now);
    if (now < 1600000000) {
        strlcpy(err, "Clock is not set yet", n);
        return false;
    }
    return true;
}

static bool take_user(cJSON *o)
{
    cJSON *id = cJSON_GetObjectItem(o, "id");
    cJSON *uid = cJSON_GetObjectItem(o, "uid");
    cJSON *nick = cJSON_GetObjectItem(o, "nickname");
    char idbuf[ECO_UID_MAX + 1];
    add_id(id ? id : uid, idbuf, sizeof idbuf);
    if (idbuf[0])
        strlcpy(s_uid, idbuf, sizeof s_uid);
    if (nick && cJSON_IsString(nick) && nick->valuestring)
        strlcpy(s_nick, nick->valuestring, sizeof s_nick);
    return s_uid[0] != 0;
}

static bool do_login(const char *acct, const char *pwd, char *err, size_t err_n)
{
    char eacct[256], epwd[256], body[640];
    if (enc_uri(acct, eacct, sizeof eacct) < 0 || enc_uri(pwd, epwd, sizeof epwd) < 0) {
        strlcpy(err, "Account or password is too long", err_n);
        return false;
    }
    snprintf(body, sizeof body, "account=%s&password=%s&authorize=", eacct, epwd);
    ck_clear();
    cJSON *o = http_json(HTTP_METHOD_POST, "https://www.ecowitt.net/user/site/login",
                         "application/x-www-form-urlencoded",
                         "https://www.ecowitt.net/home/login",
                         body, false, err, err_n);
    if (!o)
        return false;
    int code = api_code(o);
    if (code != 0) {
        api_msg(o, err, err_n, "Login failed");
        cJSON_Delete(o);
        return false;
    }
    take_user(o);
    cJSON_Delete(o);
    if (!s_uid[0]) {
        strlcpy(err, "Login did not return a user id", err_n);
        return false;
    }
    ESP_LOGI(TAG, "login uid=%s", s_uid);
    return true;
}

static bool fetch_user(char *err, size_t err_n)
{
    cJSON *o = http_json(HTTP_METHOD_POST, "https://www.ecowitt.net/user/site/get_user_info",
                         "application/x-www-form-urlencoded",
                         "https://www.ecowitt.net/home/index",
                         "", false, err, err_n);
    if (!o)
        return false;
    int code = api_code(o);
    if (code != 0) {
        api_msg(o, err, err_n, "Could not read the account");
        cJSON_Delete(o);
        return false;
    }
    take_user(o);
    cJSON_Delete(o);
    return true;
}

static void attach_devices(cJSON *dst, cJSON *list)
{
    char mine[18];
    our_mac(mine, sizeof mine);
    cJSON *arr = cJSON_AddArrayToObject(dst, "devices");
    cJSON *data = cJSON_GetObjectItem(list, "data");
    cJSON *rows = data ? cJSON_GetObjectItem(data, "rows") : NULL;
    int shown = 0;
    bool found = false;
    cJSON *row = NULL;
    if (rows && cJSON_IsArray(rows))
    cJSON_ArrayForEach(row, rows) {
        char id[ECO_ID_MAX + 1], mac[24];
        add_id(cJSON_GetObjectItem(row, "id"), id, sizeof id);
        cJSON *mac_j = cJSON_GetObjectItem(row, "mac");
        mac[0] = 0;
        if (mac_j && cJSON_IsString(mac_j) && mac_j->valuestring)
            strlcpy(mac, mac_j->valuestring, sizeof mac);
        bool mine_row = mac_eq(mac, mine);
        if (mine_row) {
            strlcpy(s_devid, id, sizeof s_devid);
            found = true;
        }
        if (shown >= 20)
            continue;
        cJSON *one = cJSON_CreateObject();
        if (!one)
            continue;
        cJSON_AddStringToObject(one, "id", id);
        cJSON *name = cJSON_GetObjectItem(row, "name");
        cJSON *model = cJSON_GetObjectItem(row, "model");
        cJSON *zone = cJSON_GetObjectItem(row, "date_zone_id");
        cJSON *online = cJSON_GetObjectItem(row, "re_time_text");
        cJSON_AddStringToObject(one, "name",
                                (name && cJSON_IsString(name) && name->valuestring) ? name->valuestring : "");
        cJSON_AddStringToObject(one, "mac", mac);
        cJSON_AddStringToObject(one, "model",
                                (model && cJSON_IsString(model) && model->valuestring) ? model->valuestring : "");
        cJSON_AddStringToObject(one, "zone",
                                (zone && cJSON_IsString(zone) && zone->valuestring) ? zone->valuestring : "");
        cJSON_AddStringToObject(one, "online",
                                (online && cJSON_IsString(online) && online->valuestring) ? online->valuestring : "");
        cJSON_AddNumberToObject(one, "mine", mine_row ? 1 : 0);
        cJSON_AddItemToArray(arr, one);
        shown++;
    }
    if (!found)
        s_devid[0] = 0;
    s_seen = found ? 1 : 0;
}

static bool fetch_devices(cJSON *dst, char *err, size_t err_n)
{
    char tbuf[16], sig[33], url[192];
    snprintf(tbuf, sizeof tbuf, "%lld", (long long)time(NULL));
    eco_kv_t kv[] = {
        { "user_id", s_uid },
        { "time", tbuf },
    };
    if (eco_sign(kv, 2, sig) != 0) {
        strlcpy(err, "Could not sign the device list", err_n);
        return false;
    }
    snprintf(url, sizeof url,
             "https://www.ecowitt.net/api/web/v1/device/getDeviceList?user_id=%s&time=%s&sign=%s",
             s_uid, tbuf, sig);
    cJSON *o = http_json(HTTP_METHOD_GET, url, NULL,
                         "https://www.ecowitt.net/home/manage",
                         NULL, true, err, err_n);
    if (!o)
        return false;
    if (api_code(o) != 0) {
        api_msg(o, err, err_n, "Could not list devices");
        cJSON_Delete(o);
        return false;
    }
    attach_devices(dst, o);
    cJSON_Delete(o);
    return true;
}

static bool ensure_session(char *err, size_t err_n)
{
    if (!s_acct[0] || !s_pwd[0]) {
        strlcpy(err, "Log in to Ecowitt first", err_n);
        return false;
    }
    if (s_nck > 0 && fetch_user(err, err_n))
        return true;
    if (!do_login(s_acct, s_pwd, err, err_n))
        return false;
    fetch_user(err, err_n);
    return true;
}

static bool name_ok(const char *name)
{
    if (!name || !name[0] || strlen(name) > ECO_NAME_MAX)
        return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == ' ' || *p == '-' || *p == '_' || *p == '.')
            continue;
        return false;
    }
    return true;
}

static cJSON *job_login(void)
{
    char err[128];
    const char *acct = s_job.account;
    const char *pwd = s_job.password;
    s_check_err[0] = 0;
    s_checked = 1;
    if (!acct[0])
        acct = s_acct;
    if (!pwd[0]) {
        if (!s_acct[0] || strcmp(acct, s_acct) != 0) {
            return result("0", "Enter the Ecowitt password");
        }
        pwd = s_pwd;
    }
    if (!acct[0] || !pwd[0])
        return result("0", "Enter the Ecowitt account and password");
    if (strlen(acct) > ECO_ACCT_MAX || strlen(pwd) > ECO_PWD_MAX)
        return result("0", "Account or password is too long");
    if (!net_ready(err, sizeof err))
        return result("0", err);
    if (!do_login(acct, pwd, err, sizeof err))
        return result("0", err);
    if (s_acct[0] && strcmp(s_acct, acct) != 0) {
        s_devid[0] = 0;
        s_seen = 0;
    }
    strlcpy(s_acct, acct, sizeof s_acct);
    strlcpy(s_pwd, pwd, sizeof s_pwd);
    fetch_user(err, sizeof err);
    nvs_save();
    cJSON *o = result("1", "Logged in");
    if (!o)
        return NULL;
    if (!fetch_devices(o, err, sizeof err))
        cJSON_AddStringToObject(o, "list_error", err);
    else
        nvs_save();
    refresh_live(o);
    return o;
}

static cJSON *job_register(void)
{
    char err[128];
    char mac[18], lat_s[24], lon_s[24], addr[64], summer[2], tbuf[16], sig[33];
    char body[768];
    double lat = 0, lon = 0;
    bool loc = false;
    char iana[40];
    const char *name = s_job.name;
    s_check_err[0] = 0;
    s_checked = 1;
    if (!name_ok(name))
        return result("0", "Device name: letters, numbers, space, . _ -");
    if (!net_ready(err, sizeof err))
        return result("0", err);
    hp10_location_get(&lat, &lon, NULL, &loc);
    if (!loc)
        return result("0", "Set latitude and longitude first");
    hp10_tz_get(iana, sizeof iana);
    if (!iana[0])
        return result("0", "Set a time zone first");
    if (!ensure_session(err, sizeof err))
        return result("0", err);

    our_mac(mac, sizeof mac);
    snprintf(lat_s, sizeof lat_s, "%.6f", lat);
    snprintf(lon_s, sizeof lon_s, "%.6f", lon);
    snprintf(addr, sizeof addr, "%.6f\xC2\xB0 %c, %.6f\xC2\xB0 %c",
             lat < 0 ? -lat : lat, lat < 0 ? 'S' : 'N',
             lon < 0 ? -lon : lon, lon < 0 ? 'W' : 'E');
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    summer[0] = tm.tm_isdst > 0 ? '1' : '0';
    summer[1] = 0;
    snprintf(tbuf, sizeof tbuf, "%lld", (long long)now);

    eco_kv_t kv[] = {
        { "name", name },
        { "addr", addr },
        { "longitude", lon_s },
        { "latitude", lat_s },
        { "type", "2" },
        { "date_zone_id", iana },
        { "is_summer_time", summer },
        { "mac", mac },
        { "imei", "" },
        { "is_public", "1" },
        { "tfp", "1" },
        { "user_id", s_uid },
        { "device_id", "" },
        { "time", tbuf },
    };
    if (eco_sign(kv, 14, sig) != 0)
        return result("0", "Could not sign the registration");
    snprintf(body, sizeof body,
             "{\"name\":\"%s\",\"addr\":\"%s\",\"longitude\":\"%s\",\"latitude\":\"%s\","
             "\"type\":2,\"date_zone_id\":\"%s\",\"is_summer_time\":\"%s\","
             "\"mac\":\"%s\",\"imei\":\"\",\"is_public\":\"1\",\"tfp\":\"1\","
             "\"user_id\":\"%s\",\"device_id\":\"\",\"time\":%s,\"sign\":\"%s\"}",
             name, addr, lon_s, lat_s, iana, summer, mac, s_uid, tbuf, sig);

    ESP_LOGI(TAG, "register name=%s mac=%s tz=%s", name, mac, iana);
    cJSON *saved = http_json(HTTP_METHOD_POST,
                             "https://www.ecowitt.net/api/web/v1/device/saveDevice",
                             "application/json",
                             "https://www.ecowitt.net/home/manage",
                             body, true, err, sizeof err);
    if (!saved)
        return result("0", err);
    if (api_code(saved) != 0) {
        api_msg(saved, err, sizeof err, "Registration failed");
        cJSON_Delete(saved);
        return result("0", err);
    }
    cJSON *data = cJSON_GetObjectItem(saved, "data");
    char new_id[ECO_ID_MAX + 1];
    add_id(data ? cJSON_GetObjectItem(data, "id") : NULL, new_id, sizeof new_id);
    cJSON_Delete(saved);
    if (new_id[0]) {
        strlcpy(s_devid, new_id, sizeof s_devid);
        s_seen = 1;
    }

    cJSON *o = result("1", "Camera registered");
    if (!o)
        return NULL;
    if (new_id[0])
        cJSON_AddStringToObject(o, "new_id", new_id);
    if (!fetch_devices(o, err, sizeof err))
        cJSON_AddStringToObject(o, "list_error", err);
    nvs_save();
    refresh_live(o);
    return o;
}

static void clear_session(void)
{
    s_acct[0] = 0;
    s_pwd[0] = 0;
    s_uid[0] = 0;
    s_nick[0] = 0;
    s_devid[0] = 0;
    s_nck = 0;
    s_seen = 0;
    s_check_err[0] = 0;
    memset(s_cks, 0, sizeof s_cks);
}

static cJSON *job_logout(void)
{
    s_checked = 1;
    clear_session();
    nvs_save();
    return result("1", "Logged out");
}

static void boot_refresh(void)
{
    char err[128];
    cJSON *o;

    err[0] = 0;
    if (!ensure_session(err, sizeof err)) {
        strlcpy(s_check_err, err, sizeof s_check_err);
        s_seen = 0;
        return;
    }
    o = cJSON_CreateObject();
    if (!o) {
        strlcpy(s_check_err, "Out of memory", sizeof s_check_err);
        s_seen = 0;
        return;
    }
    if (!fetch_devices(o, err, sizeof err)) {
        strlcpy(s_check_err, err, sizeof s_check_err);
        s_seen = 0;
    } else {
        s_check_err[0] = 0;
        nvs_save();
    }
    cJSON_Delete(o);
}

static void eco_task(void *arg)
{
    (void)arg;
    s_body = psram_malloc(16384);
    for (;;) {
        uint32_t wait = s_checked ? portMAX_DELAY : pdMS_TO_TICKS(1000);
        if (ulTaskNotifyTake(pdTRUE, wait)) {
            if (s_job.job == JOB_LOGIN)
                s_job.out = job_login();
            else if (s_job.job == JOB_REGISTER)
                s_job.out = job_register();
            else if (s_job.job == JOB_LOGOUT)
                s_job.out = job_logout();
            else
                s_job.out = result("0", "Bad request");
            memset(s_job.password, 0, sizeof s_job.password);
            xSemaphoreGive(s_done);
            continue;
        }
        if (!s_checked) {
            char err[128];
            if (net_ready(err, sizeof err)) {
                boot_refresh();
                s_checked = 1;
            } else if (++s_wait >= 180) {
                strlcpy(s_check_err, err, sizeof s_check_err);
                s_checked = 1;
            }
        }
    }
}

static cJSON *run_job(int job)
{
    s_job.job = job;
    s_job.out = NULL;
    xTaskNotifyGive(s_task);
    xSemaphoreTake(s_done, portMAX_DELAY);
    return s_job.out ? s_job.out : result("0", "Out of memory");
}

void hp10_eco_init(void)
{
    nvs_load();
    s_seen = 0;
    s_check_err[0] = 0;
    s_checked = !(s_acct[0] && s_pwd[0]);
    s_lock = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    if (!s_lock || !s_done) {
        ESP_LOGE(TAG, "semaphore failed");
        return;
    }
    if (xTaskCreate(eco_task, "eco", 16384, NULL, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s_task = NULL;
    }
}

void hp10_eco_status_json(cJSON *o)
{
    if (!o)
        return;
    cJSON_AddStringToObject(o, "status", "1");
    fill_status(o);
}

cJSON *hp10_eco_login(const char *account, const char *password)
{
    cJSON *o;
    if (!s_task || !s_lock || !s_done)
        return result("0", "Ecowitt client is not running");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_job.account[0] = 0;
    s_job.password[0] = 0;
    if (account)
        strlcpy(s_job.account, account, sizeof s_job.account);
    if (password)
        strlcpy(s_job.password, password, sizeof s_job.password);
    o = run_job(JOB_LOGIN);
    xSemaphoreGive(s_lock);
    return o;
}

cJSON *hp10_eco_register(const char *name)
{
    cJSON *o;
    if (!s_task || !s_lock || !s_done)
        return result("0", "Ecowitt client is not running");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_job.name[0] = 0;
    if (name)
        strlcpy(s_job.name, name, sizeof s_job.name);
    o = run_job(JOB_REGISTER);
    xSemaphoreGive(s_lock);
    return o;
}

cJSON *hp10_eco_logout(void)
{
    cJSON *o;
    if (!s_task || !s_lock || !s_done)
        return result("0", "Ecowitt client is not running");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    o = run_job(JOB_LOGOUT);
    xSemaphoreGive(s_lock);
    return o;
}
