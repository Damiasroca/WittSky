#pragma once

#include "esp_camera.h"

/*
 * HP10 / HP10X GPIO recovered from ESP32_HP10_V1.1.1.elf
 * (camera_init @ 400e4a34, gpio_app_init @ 400e3678).
 *
 * Camera DVP is the AI-Thinker ESP32-CAM set. The reconstructed
 * app/camera.c shifted pin_xclk — XCLK is GPIO 0, not 26.
 */

#define PIN_CAM_PWDN     32  /* stock probe ends HIGH (0→1), not the modern 1→0 */
#define PIN_CAM_RESET    -1
#define PIN_CAM_XCLK      0
#define PIN_CAM_SIOD     26
#define PIN_CAM_SIOC     27
#define PIN_CAM_D7       35
#define PIN_CAM_D6       34
#define PIN_CAM_D5       39
#define PIN_CAM_D4       36
#define PIN_CAM_D3       21
#define PIN_CAM_D2       19
#define PIN_CAM_D1       18
#define PIN_CAM_D0        5
#define PIN_CAM_VSYNC    25
#define PIN_CAM_HREF     23
#define PIN_CAM_PCLK     22

#define PIN_CAM_POWER     2   /* camera_power_on — driven 0 before init */
#define PIN_STATUS_LED   13   /* FUN_400e3000 — active-low (0 = on) */
#define PIN_BUTTON       14   /* FUN_400e393c, pull-up */
#define PIN_GPIO12_EN    12   /* gpio_app_init sets high */
#define PIN_GPIO4_FLASH   4   /* gpio_app_init sets low */

/* GPIO 13: stock F_led_status(1) ends at level 0. High = off. */
#define LED_ON_LEVEL      0
#define LED_OFF_LEVEL     1

#define HP10_CAM_XCLK_HZ        16000000
#define HP10_CAM_JPEG_QUALITY   10
/* Stock uses UXGA (0x0D). SVGA is the safer first-boot size. */
#define HP10_CAM_FRAMESIZE      FRAMESIZE_SVGA

#define HP10_AP_CHANNEL         4
#define HP10_AP_MAX_CONN        4
/* Canonical string is firmware/version.txt (ESP-IDF PROJECT_VER). */
#ifndef HP10_VERSION
#define HP10_VERSION            "WittSky_1.0.0"
#endif
#define HP10_HTTP_PORT          80
#define HP10_STREAM_PORT        81
#define HP10_WS_LOG_PORT        82
#define HP10_WS_LOG_CTRL_PORT   32770
#define HP10_WS_LOG_PATH        "/ws/log"
