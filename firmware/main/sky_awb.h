#pragma once

/* Caller holds g_cam_mu. No-op when the camera is down. */
void hp10_sky_awb_apply(void);

void hp10_sky_awb_apply_locked(void);
