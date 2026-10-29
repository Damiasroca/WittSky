#include "wx_router.h"

#include "esp_log.h"

#include <string.h>

#include "wx_awn.h"
#include "wx_eco.h"
#include "wx_tempest.h"
#include "wx_wl.h"
#include "wx_wu.h"

static const char *TAG = "overlay";

esp_err_t wx_fetch(int prov, int sub, const wx_cfg_t *cfg, ov_sample_t *out,
                   int timeout_ms, const char **why)
{
    esp_err_t err;

    if (why)
        *why = "weather fetch failed";
    if (!out)
        return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    switch (prov) {
    case OV_PROV_ECO:
        err = wx_eco_fetch(cfg, sub, out, timeout_ms, why);
        break;
    case OV_PROV_WL:
        err = wx_wl_fetch(cfg, sub, out, timeout_ms, why);
        break;
    case OV_PROV_AWN:
        err = wx_awn_fetch(cfg, out, timeout_ms, why);
        break;
    case OV_PROV_TEMPEST:
        err = wx_tempest_fetch(cfg, out, timeout_ms, why);
        break;
    case OV_PROV_WU:
        err = wx_wu_fetch(cfg, out, timeout_ms, why);
        break;
    default:
        if (why)
            *why = "no station";
        return ESP_ERR_INVALID_ARG;
    }
    if (err == ESP_OK)
        ESP_LOGI(TAG, "wx prov %d sub %d has 0x%08x", prov, sub, (unsigned)out->has);
    return err;
}
