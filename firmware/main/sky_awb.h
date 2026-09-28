#pragma once

#include <stddef.h>
#include <stdbool.h>

/* Caller holds g_cam_mu. No-op when the camera is down. */
void hp10_sky_awb_apply(void);

void hp10_sky_awb_apply_locked(void);

/* Gains written for the current mode. False in Auto: those registers are not a live meter. */
bool hp10_sky_awb_applied(int *r, int *g, int *b);

/* Auto, a built-in name, or the saved preset name. */
const char *hp10_sky_awb_label(void);

/* Starts a background match of Auto against the sky mask. */
const char *hp10_sky_awb_cal_begin(const char *name);
bool hp10_sky_awb_cal_busy(void);
void hp10_sky_awb_cal_status(char *state, size_t state_n, char *msg, size_t msg_n);
