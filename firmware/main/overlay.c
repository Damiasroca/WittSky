/* Band overlay. Sky stats stay on their own 1/8 decode of the original
 * JPEG: that pass wants a full downscaled frame, and folding it into this
 * full-resolution MCU walk would change the measurement. Callers run
 * sky stats on the camera buffer before hp10_overlay_render.
 *
 * The live MJPEG stream and the white-balance calibration grabs are not
 * overlaid. Calibration frames have to stay clean.
 */

#include "overlay.h"

#include "sdkconfig.h"

#if !CONFIG_JD_USE_ROM
#error overlay decode uses the ROM tjpgd row callback (CONFIG_JD_USE_ROM)
#endif
#include "rom/tjpgd.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "overlay_cfg.h"
#include "overlay_eco.h"
#include "overlay_enc.h"
#include "overlay_layout.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "overlay";

static StaticSemaphore_t s_mu_buf;
static SemaphoreHandle_t s_mu;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_fail;

void hp10_overlay_debug_fail(int which)
{
    s_fail = which;
}

bool hp10_overlay_active(int dest)
{
    if (!g_ov_en)
        return false;
    if (dest == OV_DEST_ECO && g_ov_clean)
        return false;
    return true;
}

void hp10_overlay_prepare(int dest, ov_sample_t *wx, bool *run)
{
    const char *why = NULL;
    int64_t t0;
    esp_err_t err;

    memset(wx, 0, sizeof *wx);
    *run = false;
    if (!hp10_overlay_active(dest))
        return;
    if (s_fail == 1) {
        ESP_LOGW(TAG, "overlay weather failed: forced");
        return;
    }
    if (g_ov_sta == 0) {
        *run = true;
        return;
    }
    t0 = esp_timer_get_time();
    if (g_ov_sta == 2)
        err = ov_eco_fetch_url(wx, g_ov_url, g_ov_timeout, &why);
    else
        err = ov_eco_fetch(wx, g_ov_host, g_ov_port, g_ov_timeout, &why);
    wx->fetch_ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "overlay weather failed after %d ms: %s",
                 wx->fetch_ms, why ? why : esp_err_to_name(err));
        memset(wx, 0, sizeof *wx);
        return;
    }
    *run = true;
}

typedef struct {
    const uint8_t *jpeg;
    size_t len;
    size_t pos;
    uint8_t *band;
    int w;
    int band_cap;
    int band_y;
    int rows;
    int y;
    int fail;
    ov_layout_t *layout;
    int64_t draw_us;
    int64_t enc_us;
} dec_t;

static void *psram(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p)
        p = malloc(n);
    return p;
}

static UINT in_cb(JDEC *jd, BYTE *buf, UINT n)
{
    dec_t *d = jd->device;
    if (d->pos >= d->len || n == 0)
        return 0;
    if (n > d->len - d->pos)
        n = (UINT)(d->len - d->pos);
    if (buf)
        memcpy(buf, d->jpeg + d->pos, n);
    d->pos += n;
    return n;
}

static void flush_band(dec_t *d)
{
    int64_t t0;
    if (d->rows <= 0 || d->fail)
        return;
    t0 = esp_timer_get_time();
    ov_band_t band = {
        .rgb = d->band,
        .frame_w = d->w,
        .y0 = d->band_y,
        .rows = d->rows,
    };
    ov_layout_draw(d->layout, &band);
    d->draw_us += esp_timer_get_time() - t0;
    t0 = esp_timer_get_time();
    for (int i = 0; i < d->rows; i++) {
        if (ov_jpg_line(d->band + (size_t)i * (size_t)d->w * 3) != ESP_OK) {
            d->fail = 1;
            break;
        }
    }
    d->enc_us += esp_timer_get_time() - t0;
    d->y = d->band_y + d->rows;
    d->rows = 0;
    /* UXGA encode holds this core for several seconds. One tick lets IDLE
     * run and pet the task watchdog. taskYIELD() would not, IDLE is lower
     * priority than httpd. */
    vTaskDelay(1);
}

static UINT out_cb(JDEC *jd, void *bitmap, JRECT *rect)
{
    dec_t *d = jd->device;
    int rw, rh;
    const uint8_t *src;
    if (d->fail)
        return 0;
    if (rect->left > rect->right || rect->top > rect->bottom)
        return 0;
    if (d->rows > 0 && rect->top != d->band_y) {
        flush_band(d);
        if (d->fail)
            return 0;
        if (d->y != rect->top) {
            d->fail = 1;
            return 0;
        }
    }
    if (d->rows == 0) {
        if (rect->top != d->y) {
            d->fail = 1;
            return 0;
        }
        d->band_y = rect->top;
    }
    rw = rect->right - rect->left + 1;
    rh = rect->bottom - rect->top + 1;
    if (rect->left + rw > d->w || rh > d->band_cap) {
        d->fail = 1;
        return 0;
    }
    src = bitmap;
    for (int row = 0; row < rh; row++) {
        int dy = rect->top - d->band_y + row;
        if (dy < 0 || dy >= d->band_cap) {
            d->fail = 1;
            return 0;
        }
        memcpy(d->band + ((size_t)dy * (size_t)d->w + (size_t)rect->left) * 3,
               src + (size_t)row * (size_t)rw * 3,
               (size_t)rw * 3);
    }
    if (rect->bottom - d->band_y + 1 > d->rows)
        d->rows = rect->bottom - d->band_y + 1;
    return 1;
}

static esp_err_t render_locked(const ov_sample_t *wx, const uint8_t *jpeg, size_t jpeg_len,
                               uint8_t **out, size_t *out_len)
{
    dec_t d;
    JDEC jd;
    uint8_t *work = NULL;
    uint8_t *jpg = NULL;
    ov_geom_t geom = g_ov_geom;
    char ts[24];
    time_t now;
    struct tm tm;
    size_t cap = 0;
    size_t jpg_n = 0;
    size_t psram0, psram1, int0, int1;
    int64_t t_all, t_dec;
    int w = 0, h = 0;
    esp_err_t err = ESP_FAIL;
    JRESULT jr;

    *out = NULL;
    *out_len = 0;
    if (!jpeg || jpeg_len < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        ESP_LOGW(TAG, "overlay skipped: not a jpeg");
        return ESP_FAIL;
    }

    memset(&d, 0, sizeof d);
    d.jpeg = jpeg;
    d.len = jpeg_len;
    work = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!work)
        work = psram(4096);
    if (!work) {
        ESP_LOGW(TAG, "overlay skipped: no work buffer");
        return ESP_ERR_NO_MEM;
    }
    jr = jd_prepare(&jd, in_cb, work, 4096, &d);
    if (jr != JDR_OK) {
        ESP_LOGW(TAG, "overlay skipped: jpeg prepare %d", (int)jr);
        free(work);
        return ESP_FAIL;
    }
    w = (int)jd.width;
    h = (int)jd.height;
    if (w < 16 || h < 16 || w > 1600 || h > 1200) {
        ESP_LOGW(TAG, "overlay skipped: frame %dx%d", w, h);
        free(work);
        return ESP_FAIL;
    }
    d.w = w;
    d.band_cap = (int)jd.msy * 8;
    if (d.band_cap < 8 || d.band_cap > 16) {
        ESP_LOGW(TAG, "overlay skipped: mcu rows %d", d.band_cap);
        free(work);
        return ESP_FAIL;
    }

    ts[0] = 0;
    now = time(NULL);
    if (localtime_r(&now, &tm))
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    if (ov_layout_build(&d.layout, &geom, w, h, wx, ts) != 0) {
        ESP_LOGW(TAG, "overlay skipped: layout");
        free(work);
        return ESP_FAIL;
    }

    psram0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    int0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_fail == 2) {
        ESP_LOGW(TAG, "overlay skipped: forced alloc failure");
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    {
        size_t biggest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        size_t pixels = (size_t)w * (size_t)h;
        size_t worst = pixels * 2;
        cap = pixels;
        if (biggest > pixels + 65536)
            cap = biggest - 65536;
        if (cap > worst)
            cap = worst;
        if (cap < pixels / 2) {
            ESP_LOGW(TAG, "overlay skipped: psram largest %u", (unsigned)biggest);
            err = ESP_ERR_NO_MEM;
            goto done;
        }
    }
    jpg = psram(cap);
    d.band = psram((size_t)w * (size_t)d.band_cap * 3);
    if (!jpg || !d.band) {
        ESP_LOGW(TAG, "overlay skipped: alloc jpg %u band %u",
                 (unsigned)cap, (unsigned)((size_t)w * d.band_cap * 3));
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    psram1 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    int1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ov_jpg_begin(w, h, jpg, cap) != ESP_OK) {
        ESP_LOGW(TAG, "overlay skipped: encoder");
        goto done;
    }

    t_all = esp_timer_get_time();
    jr = jd_decomp(&jd, out_cb, 0);
    if (jr == JDR_OK && !d.fail)
        flush_band(&d);
    t_dec = esp_timer_get_time() - t_all;
    if (jr != JDR_OK || d.fail || d.y != h) {
        ESP_LOGW(TAG, "overlay skipped: decode jr=%d fail=%d rows=%d/%d",
                 (int)jr, d.fail, d.y, h);
        ov_jpg_abort();
        goto done;
    }
    if (ov_jpg_end(&jpg_n) != ESP_OK || jpg_n < 4 || jpg[0] != 0xFF || jpg[1] != 0xD8) {
        ESP_LOGW(TAG, "overlay skipped: encode overflow cap %u", (unsigned)cap);
        goto done;
    }
    ESP_LOGI(TAG,
             "overlay %dx%d fetch %d ms decode %d draw %d encode %d jpeg %u src %u "
             "psram %u->%u (%u) internal %u->%u",
             w, h, wx ? wx->fetch_ms : 0,
             (int)((t_dec - d.draw_us - d.enc_us) / 1000),
             (int)(d.draw_us / 1000), (int)(d.enc_us / 1000),
             (unsigned)jpg_n, (unsigned)jpeg_len,
             (unsigned)psram0, (unsigned)psram1,
             (unsigned)(psram0 > psram1 ? psram0 - psram1 : 0),
             (unsigned)int0, (unsigned)int1);
    {
        /* The encoder is given the worst-case buffer. Keep only the JPEG
         * so the upload can allocate its own packet afterwards. */
        uint8_t *tight = heap_caps_realloc(jpg, jpg_n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (tight)
            jpg = tight;
        else
            ESP_LOGW(TAG, "overlay jpeg shrink to %u failed", (unsigned)jpg_n);
    }
    *out = jpg;
    *out_len = jpg_n;
    jpg = NULL;
    err = ESP_OK;

done:
    ov_layout_free(d.layout);
    free(d.band);
    free(work);
    free(jpg);
    return err;
}

esp_err_t hp10_overlay_render(const ov_sample_t *wx, const uint8_t *jpeg, size_t jpeg_len,
                              uint8_t **out, size_t *out_len)
{
    esp_err_t err;
    if (!out || !out_len)
        return ESP_ERR_INVALID_ARG;
    *out = NULL;
    *out_len = 0;
    if (!s_mu) {
        portENTER_CRITICAL(&s_init_mux);
        if (!s_mu)
            s_mu = xSemaphoreCreateMutexStatic(&s_mu_buf);
        portEXIT_CRITICAL(&s_init_mux);
    }
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(12000)) != pdTRUE) {
        ESP_LOGW(TAG, "overlay busy, using original");
        return ESP_ERR_TIMEOUT;
    }
    err = render_locked(wx, jpeg, jpeg_len, out, out_len);
    xSemaphoreGive(s_mu);
    return err;
}
