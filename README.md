# WittSky

WittSky is replacement firmware for the Ecowitt HP10 / HP10X sky camera. It runs on the camera’s ESP32, drives the onboard OV2640, and keeps the same 4 MB flash map the board shipped with. The current release string is `WittSky_1.0.0`, taken from `firmware/version.txt` and compiled in as `HP10_VERSION`.

The image is a bring-up of the board, written against the pinout, web pages, and upload protocol recovered from the stock `ESP32_HP10_V1.1.1` firmware. It is not a line-by-line port of that binary’s task graph. What it does carry across is the behaviour that matters in the field: the camera comes up without browning out the rail, the access point is `HP10-WIFI` plus the last two MAC bytes, pictures can be posted to Ecowitt or to a server you choose, and the on-device pages still look and talk like the camera’s own UI.

On top of that recovered behaviour the firmware adds a sky measurement (cloud fraction, colour ratio, exposure, a relative light index), an application watchdog with a reboot cap, mDNS, a full IANA time-zone table, a WebSocket log stream, and two ways to install a new image (a version URL, or an app `.bin` uploaded from the browser).

The ESP-IDF project lives in `firmware/`. The CMake project name is `hp10_bringup`, so the application binary is `hp10_bringup.bin`.

## Hardware

The board is an ESP32 with external PSRAM and a 4 MB flash. The sensor is an OV2640 (the stock probe identified it as DZ0223). The parallel camera bus is the familiar AI-Thinker ESP32-CAM set, with one important difference recovered from `camera_init` in the V1.1.1 ELF: the sensor clock, XCLK, is GPIO 0. It is not GPIO 26. GPIO 26 is the SCCB data line.

| Signal | GPIO | Role in this firmware |
| --- | --- | --- |
| XCLK | 0 | 16 MHz sensor clock, LEDC timer 0 / channel 0 |
| SIOD / SIOC | 26 / 27 | SCCB, legacy I2C driver, port 1, 200 kHz |
| D0–D7 | 5, 18, 19, 21, 36, 39, 34, 35 | 8-bit DVP data |
| PCLK / HREF / VSYNC | 22 / 23 / 25 | Pixel clock and sync |
| PWDN | 32 | Owned by this firmware, not by `esp32-camera` |
| Camera rail | 2 | High turns the rail off. Driven low only after Wi-Fi PHY calibration |
| Status LED | 13 | Active low. On after a successful sensor init, off if init fails |
| Button | 14 | Input with pull-up, matching the stock board. The application does not read it |
| GPIO 12 | 12 | Held high, as the stock `gpio_app_init` did |
| GPIO 4 | 4 | Held low. There is no user-facing flash control |

`esp32-camera` is told `pin_pwdn = -1` so its own power-down sequence cannot invert the polarity this board needs. The stock sequence ends with PWDN high: drive the opposite level, wait 200 ms, drive high, wait 500 ms. If `esp_camera_init` still fails, the firmware deinitialises, pulses PWDN the other way, and tries once more.

The CPU runs at 240 MHz. PSRAM is initialised at boot, 40 MHz, and used for the frame buffer when it is present. If PSRAM is missing, the frame buffer falls back to internal RAM and any saved resolution above VGA is clamped to VGA. `CONFIG_SPIRAM_IGNORE_NOTFOUND` is set so a board that fails the PSRAM probe can still boot.

## Why the boot order is rigid

PHY calibration is a short, sharp current spike. On this board a 20 dBm calibration is enough to sag the rail, trip the brownout detector, and reset the chip. Two things keep that from happening.

The maximum Wi-Fi transmit power is 15 dBm, both in `sdkconfig.defaults` (`CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=15`) and again at runtime with `esp_wifi_set_max_tx_power(60)` (the API takes quarter-dBm units, so 60 is 15 dBm). The Kconfig value is what the PHY uses during calibration. The runtime call covers beacons and data after that.

The camera rail stays off until calibration has finished. `app_main` waits one second, configures GPIO with the rail high, starts the radio, waits another 300 ms, and only then calls `hp10_camera_boot`. If the sensor never answers, the access point and the web server still come up. A dead camera does not take the whole device with it.

After the camera attempt, startup continues in this order:

1. NVS, then configuration, the Ecowitt account task, the health record, and the camera mutex.
2. GPIO, then the SoftAP in AP+STA mode.
3. Camera, with retries if the watchdog is enabled.
4. HTTP on port 80 and the MJPEG server on port 81.
5. SNTP and the sun calculation.
6. The upload task.
7. The WebSocket log task.
8. The SoftAP policy task, and a station restore if an SSID was saved.

Wi-Fi credentials live in RAM (`WIFI_STORAGE_RAM`). NVS is the only copy that survives reboot, and the firmware writes it itself.

## Wireless

The radio is always started in `WIFI_MODE_APSTA`, with power save off. The station MAC is read once and used for the access-point name, Ecowitt identity, and the status page.

### Access point

The SSID is `HP10-WIFI` followed by the last two bytes of the station MAC, printed as two uppercase hex bytes. Channel is 4. At most four clients are accepted. With no password configured the network is open. With a password it is WPA2-PSK.

The address is `192.168.4.1`. That is both the address the firmware announces in its own log and the address the DHCP server on the AP interface hands out as the gateway. The first page to open is `http://192.168.4.1/`, which redirects to the login page.

The password, when set, is 8 to 63 characters drawn from letters, digits, and the apostrophe. An empty password is valid and means an open network. Changing it reboots the camera, because the AP configuration is applied from the saved value at the next start, and the login check uses the same string.

### When the access point turns off

Two saved flags decide this, and a third piece of live state can override both.

`apOn` is the operator’s preference. Turning the access point off from the Network page clears it. The radio actually leaves AP mode only while the station already has an IP address. If the camera is not on a router yet, the access point stays up and the page says so. Dropping the router link brings the access point back, even if the saved preference is off. That is deliberate: a camera that has lost its router and also has no access point cannot be recovered without a serial cable.

`apAuto` is the five-minute timer. Once the station has held an IP for 300 seconds, and the preference is still “on”, and auto-off is enabled, the access point is shut down. Any loss of the station IP starts the access point again and clears the timer.

A one-second task (`ap_auto`) applies those rules. Mode changes take the AP mutex so they cannot race a manual on/off from the web server. The page also keeps a 12-line log of client associations, disassociations, and DHCP leases, timestamped from the monotonic boot clock.

### Station

Joining a router is a blocking call from the web handler, with a bounded wait, so the Network page can report a real result:

| `status` | Meaning |
| --- | --- |
| `0` | Associated and an IP was assigned. SSID and password are then saved. |
| `1` | The SSID was not found before the wait expired. |
| `2` | Authentication failed, the 4-way handshake timed out, or a related reason code (including 200 and 202–204). |
| `3` | Some other disconnect, or the wait expired without a clearer cause. |
| `4` | No SSID was sent, or `esp_wifi_connect` itself failed. |

Before the connect, the firmware runs a targeted scan for that SSID, including hidden networks. If the scan returns a record, the connect is pinned to that BSSID and channel. The scan method is all-channel, sorted by signal, with the driver’s own failure retry count set to 8 for a user-initiated join. The minimum accepted auth mode is open, so the driver will still associate to WPA2 or WPA3-transition networks; it is not restricted to open APs.

A failed join does not leave the driver sitting on the SSID that just failed. The saved SSID is put back and the background rejoin task is kicked.

That task (`sta_link`) is the long-term link. If a saved SSID exists and the station has no IP, it disconnects a stale association, rewrites the station config from NVS, and connects. Success clears the backoff. Failure doubles the wait, starting at 1 second and capping at 60 seconds. While it is waiting, the link state reported on the status page is `backoff`. The other states are `idle` (no SSID saved), `rejoining`, and `up`.

A spontaneous disconnect, or a lost IP, kicks the same task, unless a user join is already in progress. The user join sets a flag so the background task does not fight it.

The ESP32 station is 2.4 GHz only. A dual-band router has to be joined on the 2.4 GHz SSID.

### mDNS

Once the station has an address, the firmware advertises `http://<hostname>.local` as an `_http._tcp` service named HP10, on port 80. The default hostname is `camera`. A replacement must be 1–31 characters, letters, digits, and hyphens, and must not start or end with a hyphen. Two cameras with the same name on one LAN will collide; mDNS does not uniquify the name for you.

## Login

The login page shows a disabled user field set to `admin`. The firmware never reads it. The page posts only the password to `/set_login_info`, base64-encoded. The encoding is only what the page’s `baseCode()` helper does; it is not a secret. The firmware compares the decoded string with the access-point password.

If that password is empty, every login succeeds. If it is set, a mismatch clears the logged-in flag and returns “Wrong password”.

The logged-in flag is a single boolean in RAM, `g_bLoggedIn`. It is not a cookie and it is not per client. There is no local logout. The first successful login authorises every browser that can reach the camera, until reboot. Protected HTML pages answer a guest with a redirect to `/login.html`. Protected JSON routes answer `401`.

A few routes stay open because the live view and the version string are used before, or outside, that check: `/get_version`, `/login.html`, `/video.html`, `/axcss.css`, `/axjs.js`, `/get_video_info`, `/set_video_info`, `/set_video_cfg`, `/capture`, and `/stream` on port 81. Everything else in the API table below requires the flag.

## The web interface

Pages are compiled into the application image. There is no SPIFFS website to update separately. The sidebar, shared by every page except login, links Status, Network, Capture, Sky, Camera, and System. A strip along the top of those pages polls `/get_health` every 15 seconds and shows RSSI, uptime, the last upload, and whether the camera is up. A `401` hides the strip.

### Status

RSSI is labelled good at −60 dBm and above, fair down to −75 dBm, and weak below that. The page also shows the station SSID and IP, camera state, age and result of the last upload, MAC, uptime, free internal heap, free and total PSRAM, the ESP-IDF reset reason, and the previous watchdog cause if one was stored in RTC memory.

Two actions sit on this page. **Upload now** asks the upload task for a single shot and waits up to 50 seconds. **Retry camera** deinits the sensor if it was marked up, then runs `camera_bringup` up to `wd_cam_n` times (at most five).

### Network

Hostname, router SSID and password, the address the DHCP server assigned, and the access-point controls. Scan returns up to 16 networks with auth mode and RSSI. The station password is base64-encoded on the way in, the same way the login password is, and it is never echoed back: `/get_network_info` returns an empty `wifi_pwd`.

### Capture

This page is the schedule and the destination.

The clock block shows local date and time, the IANA zone with its current UTC offset and a DST marker, and today’s sunrise and sunset in local minutes. Sunrise and sunset stay at `00:00` until a location has been saved.

Latitude and longitude are stored as microdegrees in NVS (`lat_e6`, `lon_e6`). The zone is an IANA name. The firmware looks it up in a generated table (`tz_zones.c`, built by `gen_tz_zones.py` from the [nayarsystems/posix_tz_db](https://github.com/nayarsystems/posix_tz_db) zone list) and installs the matching POSIX `TZ` string with `setenv` and `tzset`. DST then follows that rule, including the last Sunday of March and October in Europe and the US transition dates, because those rules are inside the POSIX string. Names longer than 39 characters, or POSIX strings longer than 63, are dropped when the table is generated.

The Ecowitt account block logs into `www.ecowitt.net`, lists devices, and can register this camera. Upload settings are separate and are described below.

### Sky

The latest measurement, a still frame with the mask rectangle drawn over it, and the settings that produced the measurement. The still is a new capture. It is not the frame that was measured. **Measure now** forces a computation and is limited to one request every five seconds; a second request in that window is HTTP 429.

Sky statistics are off until you enable them. With the defaults, the mask is the top half of the frame (`x=0`, `y=0`, `w=100`, `h=50`), the red/blue threshold is 0.600, saturated pixels are those at or above 250 on any channel, white balance is automatic, and cloud cover is withheld for 30 minutes after sunrise and 30 minutes before sunset.

### Camera

A live MJPEG view from port 81, plus resolution, brightness, contrast, saturation, horizontal mirror, and vertical flip.

Moving a control posts `/set_video_info` immediately and updates the running sensor. **Save** posts `/set_video_cfg`, which is the set that is written to NVS and reapplied after reboot. Resolution is chosen when `esp_camera_init` runs. Saving a different size is stored at once, and the page tells you to reboot before that size is actually used. Brightness, contrast, and saturation are applied on the next boot only when they are non-zero. Calling the OV2640 “set brightness” path with zero rewrites the sensor’s special-digital-effects block and leaves the picture too bright, so a stored zero is left as the sensor default.

JPEG quality stays at 10 (the esp32-camera scale, where lower is better). The quality slider on the classic camera page is not shown.

Supported sizes are QQVGA 160×120, HQVGA 240×176, QVGA 320×240, CIF 400×296, VGA 640×480, SVGA 800×600, XGA 1024×768, SXGA 1280×1024, and UXGA 1600×1200. The first boot, before anything has been saved, uses SVGA. The stock camera used UXGA. Without PSRAM the init size is VGA at most.

The frame pipeline is a single JPEG buffer, in PSRAM when available, grabbed with `CAMERA_GRAB_WHEN_EMPTY`. One buffer means a stream client, a capture, an upload, and a sky measurement all take the same mutex, `g_cam_mu`. They do not run in parallel. SCCB register access also has to stay on that mutex: the sensor driver caches the current register bank in a static.

### System

Watchdog thresholds, the WebSocket log, OTA, reboot, and factory reset. Factory reset erases the `hp10` NVS namespace, clears the RTC watchdog record, and reboots. It does not erase the Wi-Fi PHY calibration data in the `phy_init` partition.

## Clock and the sun

SNTP starts the first time the station has an IP. Servers are `pool.ntp.org`, `time.windows.com`, and `time.nist.gov`. The task checks every three seconds. If the station is up and the clock is still before Unix time 1600000000 (2020-09-13) for ten consecutive checks, about half a minute, SNTP is restarted. That covers a join that won the race against the first DNS lookup and then sat in the long SNTP retry.

Until the clock is real, sunrise math and the time-zone offset preview use 2026-01-20 as a stand-in so the zone string can still be displayed. Ecowitt login, device registration, and a trustworthy `dateutc` all refuse to proceed until SNTP has actually succeeded. An Ecowitt image upload will still be attempted, but `dateutc` is sent as the Unix epoch and the log says the server may reject it.

Sunrise and sunset use the Ed Williams / USNO approximation with a zenith of 90.833°, which includes average atmospheric refraction. The result is local minutes for the saved latitude, longitude, and the current UTC offset of the selected zone. The calculation is repeated when the local day changes and when the DST flag flips. Latitudes where the sun does not cross that zenith (midnight sun, polar night) make `sun_rise_set` return false, and the sky “day” flag stays unknown rather than guessed.

The sky daylight window is those local minutes, shrunk by the daylight margin: at or after sunrise plus the margin, and at or before sunset minus the margin. The default margin is 30 minutes. A margin of 0 uses civil sunrise and sunset as the window edges.

## Uploads

One destination, or none. Custom URL and Ecowitt cannot both be enabled. If a stored config somehow has both, the custom flag is cleared on load. Both are off by default. A custom URL has to be `http://` or `https://`, at most 127 characters, and it must not contain `ecowitt.net`. Ecowitt image upload has its own hosts and does not go through that URL.

The interval control is the stock `ost_interval` value:

| Value | Period |
| --- | --- |
| 0 | Off. The task stays idle. **Upload now** can still fire a shot. |
| 1 | 5 minutes |
| 2 | 10 minutes |
| 3 | 15 minutes |
| 4 | 20 minutes |
| 5 | 25 minutes |

When a destination and a non-zero interval are both set, the first picture goes out about five seconds later, then on the period. If the station has no IP, or the camera is down, the shot is skipped and retried in ten seconds. A camera that is down is asked to recover first. Each shot grabs one JPEG under the camera mutex, optionally computes sky stats, posts, and releases the frame.

The task stack is 12 KB. The idle reason is logged on change and at least every 30 seconds: interval off, both destinations, upload disabled, bad URL, camera down, or no station IP.

### Custom server

`POST` of the JPEG body, `Content-Type: image/jpeg`, 30 second timeout. HTTPS uses the ESP-IDF certificate bundle. A second attempt follows a failure, after two seconds, with the timeout cut to 12 seconds, and only if the station still has an address. HTTP status outside 200–299 is a failure.

If sky stats are enabled and the measurement fits in the buffer, the same JSON is sent in the `X-Sky-Stats` header. The body stays a plain JPEG, so a server that only stores the body still receives a picture.

### Ecowitt image upload

The body is `multipart/form-data` with boundary `----DataPackageBoundary`, posted to one of:

- `http://rtpmedia.ecowitt.net/data/upload_image`
- `http://cdnrtpmedia.ecowitt.net/data/upload_image`

The firmware alternates hosts after a failure, and if the station is still up it tries the current host once more after a two-second pause. A response whose `errcode` is greater than zero is a failure even when HTTP itself returned 2xx.

Fields, in order:

| Field | Value |
| --- | --- |
| `mac` | Station MAC, lowercase, colon-separated |
| `PASSKEY` | Uppercase hex MD5 of the MAC printed in uppercase |
| `stationtype` | Firmware version, currently `WittSky_1.0.0` |
| `model` | `HP10` |
| `dateutc` | UTC `YYYY-MM-DD HH:MM:SS` |
| `battery` | `0` |
| `reupload` | A counter incremented on every attempt since boot |
| `interval` | The period in minutes (5, 10, 15, 20, or 25) |
| `md5` | Uppercase hex MD5 of the JPEG bytes |
| `weather_image` | The JPEG, filename `<unix-time>.jpg` |

Sky stats are computed on this path too, when enabled, so the Sky page and the health snapshot update. They are not attached to the Ecowitt body. Ecowitt’s image API does not take that header.

The passkey is derived from the MAC. It is not the account password. The account password is used only by the separate login and registration client.

### Ecowitt account

Registration is what makes those MAC-addressed uploads show up under an Ecowitt.net user. The client is its own FreeRTOS task (`eco`, 16 KB stack) with a 16 KB response buffer, preferably in PSRAM. The web handlers only enqueue a job and wait.

Login is `POST https://www.ecowitt.net/user/site/login` as form fields `account`, `password`, and an empty `authorize`. Cookies from `Set-Cookie` are kept in RAM and replayed. The user id and nickname come from the login body and from `get_user_info`. Account, password, uid, nickname, and device id are stored in NVS. Logout clears all of them.

Requests that the website signs are signed the same way here. Each value is percent-encoded, with spaces as `+`. The `key=value` pairs are sorted, joined with `&`, and the literal `@ecowittnet` is appended. That suffix is part of the hash and is not sent. The signature is the uppercase hex MD5 of the resulting string. The client also sends `accept-ecowittlang: en` and `web-version: v1.228_03_11` on those API calls, plus a browser user agent.

Device list is `GET /api/web/v1/device/getDeviceList`, signed over `user_id` and `time`. The camera’s own row is the one whose `mac` matches, case-insensitively. Up to twenty rows are returned to the page, with the matching row flagged `mine`.

Registration is `POST /api/web/v1/device/saveDevice`. It requires a logged-in session, a saved location, a saved time zone, and a synced clock. The device name is 1–31 characters from letters, digits, space, and `.` `_` `-`. The suggested name is the mDNS hostname. The JSON body carries the name, a short address string built from the coordinates, longitude, latitude, `type` 2, the IANA zone, a daylight-saving flag from `tm_isdst`, the MAC, an empty IMEI, `is_public` 1, `tfp` 1, the user id, an empty `device_id`, the Unix time, and the signature.

On boot, if an account and password are already stored, the task waits until the station has an IP and the clock is set, then logs in and refreshes the device list. If that readiness never arrives, it gives up after about three minutes and records the reason in `check_error`. The page treats the camera as registered only when that live check has seen this MAC. A device id left over in NVS from an earlier account is not shown as current until the list confirms it.

## Sky measurement

`hp10_sky_compute` turns one JPEG into a JSON document, version field `v` = 1, and keeps the latest copy (up to 512 bytes) for `/get_sky_stats`, the health snapshot, and the custom-upload header.

### Decode

The JPEG is decoded with `esp_jpeg` to RGB888 at scale 1/8. A UXGA frame therefore becomes 200×150, and SVGA becomes 100×75. The decoder’s output buffer is refused if it would exceed 256 KB. Width and height reported alongside the metrics are the original frame size when the camera driver provided it, and the JPEG header size otherwise. `dec` in the JSON is the scaled size the math actually ran on.

### Luma and sharpness

Every decoded pixel is converted to 8-bit luma with the integer BT.601 weights:

```text
Y = (77·R + 150·G + 29·B) >> 8
```

`luma` is the mean of Y over the whole decoded frame, not only the mask.

Sharpness is the variance of a 4-neighbour Laplacian over the interior of that luma image:

```text
L(x, y) = Y(x−1, y) + Y(x+1, y) + Y(x, y−1) + Y(x, y+1) − 4·Y(x, y)
sharp   = variance(L)
```

The border is skipped, so a frame smaller than 3×3 reports `sharp` as null. The value is a focus and texture figure for the whole view. It is not restricted to the sky mask, and it is not a modulation-transfer-function measurement.

### The mask and the cloud ratio

The mask is an axis-aligned rectangle in percent of the decoded frame. Pixels outside it do not affect saturation, cloud count, or the red/blue mean. They do affect luma and sharpness.

Inside the mask, a pixel with R, G, or B at or above the saturation threshold (default 250, allowed 200–255) is counted in `sat_n` and then left out of the cloud sample. A pixel with B = 0 and R = 0 is ignored. A pixel with B = 0 and R > 0 is treated as cloud and does not enter the red/blue average, because the ratio is undefined. Every other pixel is usable: its R/B is accumulated, and it counts as cloud when

```text
R · 1000 ≥ threshold · B
```

The threshold is the configured `sky_rb` value. The default 600 means an R/B of 0.600. The legal range is 200–2000, that is 0.200–2.000. Clear blue sky sits well below a 0.6 ratio. Cloud and haze push the ratio up because cloud is closer to neutral and the red channel catches up with blue.

`sat_pct` is saturated pixels over the full mask area, in percent. `cloud_pct` is cloud pixels over usable pixels, in percent. `rb_mean` is the mean R/B of the pixels that had a non-zero blue channel. Those three fields are emitted only when the sample is considered usable:

- the sensor special-effect register is 0 (no tint, negative, or grayscale),
- the daylight flag is true,
- the mask is non-empty,
- usable pixels are at least 5% of the mask (`usable_n · 20 ≥ sky_px`).

Otherwise the fields are JSON null and a reason is logged. Night, twilight outside the daylight margin, a mask full of blown highlights, or a mask that landed on a roof all produce a document that still carries luma, exposure, and sharpness, with the cloud numbers withheld.

The daylight flag itself is JSON null when location, clock, or the sun calculation is missing, `true` inside the margin window, and `false` outside it. Cloud numbers require `true`. A null day does not invent a daytime result.

### What the sensor contributes

While the camera mutex is held, the firmware reads OV2640 registers directly. The high byte passed to `get_reg` selects the bank: `0x01` is the sensor bank, `0x00` is the DSP bank.

| Register | Address passed in | Bits | Use |
| --- | --- | --- | --- |
| `REG04` | `0x104` | `[1:0]` | AEC low bits |
| `AEC` | `0x110` | `[7:0]` | AEC mid bits |
| `REG45` | `0x145` | `[5:0]` | AEC high bits |
| `GAIN` | `0x100` | `[7:0]` | Analogue gain |
| AWB R, G, B | `0x0CC`, `0x0CD`, `0x0CE` | `[7:0]` | Current white-balance gains |

Exposure is reassembled as a 16-bit-class line count:

```text
aec = (REG45 << 10) | (AEC << 2) | REG04[1:0]
```

Gain follows the OV2640 coarse/fine split. Bits 7 through 4 each contribute a factor of 1 or 2, and the low nibble is a 1/16 fine step:

```text
gain_x = (bit7+1) · (bit6+1) · (bit5+1) · (bit4+1) · (1 + (GAIN & 0x0F) / 16)
```

When luma, `aec`, and `gain_x` are all positive, a relative light index is

```text
light_idx = log2( luma / (aec · gain_x) )
```

This is not lux. It is a way to compare frames after the automatic exposure controller has done its job: the same scene light should land near the same index whether the sensor answered with a short exposure or a long one. It moves when the scene does, and also when you change brightness, contrast, or the white-balance preset, because those change luma without being exposure.

White-balance gains are read only when the sky white-balance mode is not Auto. Auto leaves the gains null.

The sky white-balance control is applied to the sensor as white balance on, AWB gain on, and `wb_mode` set to the chosen preset: 0 Auto, 1 Sunny, 2 Cloudy, 3 Office, 4 Home. A fixed preset changes the colour of every picture the camera then uploads. Auto does not pin the gains. The mode is applied at camera init and again whenever the setting changes.

### The JSON document

A complete daytime sample looks like this. Nulls appear in place of the sensor or cloud fields when those reads are refused.

```json
{
  "v": 1,
  "ts": 1760000000,
  "fw": "WittSky_1.0.0",
  "frame": [800, 600],
  "dec": [100, 75],
  "luma": 92.4,
  "aec": 8400,
  "gain_reg": 0,
  "gain_x": 1.00,
  "light_idx": -6.51,
  "awb_mode": 0,
  "awb_gains": null,
  "img": { "bri": 0, "con": 0, "sat": 0, "fx": 0 },
  "img_default": true,
  "day": true,
  "mask": [0, 0, 100, 50],
  "sky_px": 3750,
  "sat_pct": 1.2,
  "cloud_pct": 34.5,
  "rb_mean": 0.812,
  "sharp": 18.6,
  "ms": 40
}
```

`ts` is Unix time, or 0 if the clock is not synced. `img` is the sensor’s brightness, contrast, saturation, and special-effect status at measurement time. `img_default` is true only when all four are zero. `ms` is the time spent inside the computation, in milliseconds. `sky_px` is the mask area in decoded pixels.

`/get_sky_stats` returns the cached document. `?fresh=1` runs a new one, if sky stats are enabled and the five-second guard allows it. `/get_health` embeds the cache under `sky`, or JSON null when nothing has been measured yet.

## Health and the watchdog

`/get_health` is the snapshot behind the status page and the sidebar strip: RSSI and label, uptime, free heap, PSRAM size and free PSRAM, camera flag, SSID, station IP, link state (`idle`, `rejoining`, `backoff`, `up`), reset reason, previous watchdog cause, last upload age and message, the camera and upload failure streaks, watchdog settings, how many automatic reboots have been recorded inside the current window, whether the watchdog is currently holding off, MAC, mDNS hostname, the access-point block (`ap_on`, `ap_hold`, SSID, channel, IP, clients, leases, log), and the latest sky JSON.

The watchdog is an application policy, separate from the ESP-IDF interrupt and task watchdogs. It is on by default. Three counters matter.

Camera failures (`hp10_wd_note_camera_fail`) increment on a failed init or a failed grab. A successful init or grab clears the streak. When the streak reaches `wd_cam_n` (default 3, range 1–20), the firmware asks for a reboot.

Upload failures work the same way with `wd_up_n`. A successful custom or Ecowitt post clears that streak. A skipped shot because the station has no IP does not count as an upload failure. A grab failure during an upload counts as a camera failure.

Reboots are capped. Timestamps of automatic reboots are kept in RTC memory (magic `0x48313057`, up to eight stamps) along with a short cause string. A reboot is allowed only when fewer than `wd_cap_n` stamps (default 3) fall inside the last `wd_cap_m` minutes (default 60, range 1–1440). Past the cap, the camera stays up, sets a held-off flag, and logs it. The status page shows that state so a camera that has given up restarting is visible instead of silently boot-looping.

Power-on and brownout resets clear the RTC record. Other reset reasons keep it, which is how “previous cause” survives the reboot the watchdog just triggered. Causes you will actually see include `camera_init`, `camera_grab`, and `upload`.

At boot, if the watchdog is enabled, camera init is retried `wd_cam_n` times, and at most eight times, with 250 ms between attempts. A streak that hits the threshold during those attempts schedules a reboot. `app_main` still goes on to start the web server, and the reboot task waits 1.5 seconds, so the UI can appear briefly and then the camera resets. With the watchdog disabled there is a single attempt, and a failure is only a log line.

A scheduled reboot, whether from the watchdog, the System page, a password change, factory reset, or a finished OTA, waits 1.5 seconds and then calls `esp_restart`.

## Firmware update

There are two install paths. Both write the inactive OTA slot (`ota_0` or `ota_1`, 1792 KB each) and set it as the next boot partition. Neither path performs an unattended upgrade on a timer. The “Automatically upgrade” checkbox on the System page is loaded from a field the firmware always reports as 0, and saving the page does not schedule anything. Check and upgrade are explicit.

### Version URL

You store an `http://` or `https://` URL. **Check firmware** `GET`s it, with a 15 second timeout, and parses a JSON object. A leading byte-order mark or other prefix is tolerated; parsing starts at the first `{`.

```json
{
  "code": 0,
  "data": {
    "version": "WittSky_1.1.0",
    "content": "Short release note",
    "attach1file": "http://192.168.1.10/hp10_bringup.bin"
  }
}
```

`code` must be 0 and `data` must be present. `attach1file` must be an `http` or `https` URL. If `version` is empty or equal to the running `HP10_VERSION`, the result is “no update”. Any other version is offered, with `content` as the note. **Upgrade Version** downloads that file with `esp_https_ota` (plain HTTP is allowed by `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP`), checks the image header, and reboots on success. The page polls `upgrade_process` with action `running` and reads progress from bytes written over image size.

The check itself does not compare numeric versions. Any string that differs from the running version is treated as an update, including a string that sorts lower.

### File from the browser

**Flash file** posts the bytes to `/upgrade_upload`. The firmware checks the first byte before it commits to `esp_ota_begin`. An ELF (`0x7F 'E' 'L' 'F'`) is rejected with “That is an ELF, not an app .bin”. Anything other than the ESP32 image magic `0xE9` is rejected as not an app image. The size must be at least 512 bytes and must fit the inactive slot. A short body is “Upload truncated”. `esp_ota_end` verifies the image; only then is the boot partition switched, and the camera reboots.

Flash the application binary, `hp10_bringup.bin`, or a dump of a single OTA slot. Do not flash a full 4 MB chip dump through this route. The partition table, NVS, and PHY data are not part of an app image, and a full dump is not a valid image for `esp_ota_write`.

## WebSocket logs

Logging stays on the UART through the normal `vprintf`. When the feature is enabled, the same lines are also copied into a 4 KB ring, if the line’s level is at or below the selected level and the tag is not one of `ws_log`, `websocket_client`, `httpd_ws`, or `httpd`. Those tags are dropped so the transport does not log itself into a loop.

The level follows ESP-IDF: 1 Error, 2 Warning, 3 Info, 4 Debug, 5 Verbose. Enabling the stream also raises the global `esp_log` level to that value, which is why verbose logging is compiled in (`CONFIG_LOG_MAXIMUM_LEVEL_VERBOSE`). Turning the stream off returns the global level to Info.

The listener is a separate HTTP server on port 82, with its control port at 32770, and at most three clients. The default path is `/ws/log`. A path you type (`/something`, letters, digits, `/`, `_`, `-`, `.`) replaces it. A `ws://` or `wss://` URL does not replace the listener; the camera still serves the local socket, and it also opens that URL as a client and pushes the same text. `wss://` uses the certificate bundle. The client reconnects on its own, and the log task restarts a dead listener or a dead client about every ten seconds while the station has an IP.

The System page opens the local socket in the browser and prints the lines. The log task does not take the camera mutex and does not run on the port 80 or port 81 servers.

## What is stored

All of the settings below live in the NVS namespace `hp10`. Wi-Fi PHY calibration stays in the `phy_init` partition and is left alone by factory reset.

| Key | Contents |
| --- | --- |
| `ssid`, `pwd` | Router credentials |
| `appwd` | Access-point and login password |
| `ap_auto`, `ap_on` | Five-minute auto-off, and the saved on/off preference |
| `mdns_host` | Hostname, default `camera` |
| `up_en`, `eco_en`, `ost`, `up_url` | Custom enable, Ecowitt enable, interval index, custom URL |
| `ota_url` | Version-check URL |
| `wsl_en`, `wsl_lvl`, `wsl_url` | WebSocket log enable, level, path or remote URL |
| `wd_en`, `wd_cam_n`, `wd_up_n`, `wd_cap_n`, `wd_cap_m` | Watchdog |
| `loc`, `lat_e6`, `lon_e6`, `utc_off`, `tz_iana` | Location and time zone |
| `eco_acct`, `eco_pwd`, `eco_uid`, `eco_nick`, `eco_devid` | Ecowitt session |
| `sky_en`, `sky_x`, `sky_y`, `sky_w`, `sky_h`, `sky_rb`, `sky_sat`, `sky_awb`, `sky_daym` | Sky mask and thresholds |
| `cam_set`, `cam_fs`, `cam_bri`, `cam_con`, `cam_sat`, `cam_hm`, `cam_vf` | Picture settings, present only after a save |

Out of range values are ignored on load and the compiled default is kept. A custom URL that fails validation disables custom upload. A bad WebSocket URL clears the URL and disables the stream. A bad OTA URL is cleared.

The Ecowitt password is stored in NVS in plaintext, as the router password is. NVS on this flash is not encrypted.

## Flash map

The table is the stock HP10 V1.1.1 layout, dumped from a live camera at offset `0x8000`. App slots are OTA slots, not a factory partition.

| Name | Type | Offset | Size |
| --- | --- | --- | --- |
| `nvs` | data | `0x9000` | 16 KB |
| `otadata` | data | `0xD000` | 8 KB |
| `phy_init` | data | `0xF000` | 4 KB |
| `ota_0` | app | `0x10000` | 1792 KB |
| `ota_1` | app | `0x1D0000` | 1792 KB |
| `custom` | FAT | `0x390000` | 192 KB |
| `storage` | SPIFFS | `0x3C0000` | 256 KB |

This firmware does not mount `custom` or `storage`. They are left in the table so the layout stays compatible with the board and with images that expect those offsets. The web UI is linked into the app image.

## HTTP API

Port 80 unless noted. Bodies are JSON, capped at 4 KB, except `/upgrade_upload`, which is the raw image.

| Method and path | Auth | Role |
| --- | --- | --- |
| `GET /` | open | Redirect to `/login.html` |
| `GET /login.html`, `/video.html`, `/axcss.css`, `/axjs.js` | open | Static UI |
| `GET /localNetwork.html`, `/status.html`, `/capture.html`, `/system.html`, `/skystats.html` | login | Static UI |
| `GET /get_version` | open | `{ version, newVersion }` |
| `POST /set_login_info` | open | `{ pwd }` base64. Returns `status` `1` or `0` |
| `GET /get_ws_settings` | login | Clock, location, upload, WebSocket log |
| `POST /set_ws_settings` | login | Interval, destinations, URL, log, lat, lon, `tz_iana` |
| `GET /get_timezones` | login | `{ zones: [ ...IANA names ] }` |
| `GET /get_network_info` | login | SSID, empty password, IP, mask, gateway, hostname |
| `POST /set_network_info` | login | `{ ssid, wifi_pwd }`. `status` is the join code above |
| `GET /usr_scan_ssid_list` | login | Up to 16 APs |
| `GET /get_device_info` | login | OTA URL, AP flags, watchdog, whether a password is set |
| `POST /set_device_info` | login | Watchdog, hostname, OTA URL, AP flags, password, reboot, factory reset |
| `GET /get_video_info` | open | Sensor status, or the saved picture settings if the sensor is down |
| `POST /set_video_info` | open | Live sensor tweaks. Not persisted |
| `POST /set_video_cfg` | open | Persist resolution, brightness, contrast, saturation, mirror, flip |
| `GET /capture` | open | One JPEG |
| `GET :81/stream` | open | `multipart/x-mixed-replace` MJPEG |
| `GET /get_health` | login | Health snapshot |
| `POST /upload_now` | login | One immediate upload |
| `POST /retry_camera` | login | Re-init the sensor |
| `POST /upgrade_process` | login | `upgrade` = `check`, `start`, `running` / `progress`, `over` / `reboot` |
| `POST /upgrade_upload` | login | Raw app image |
| `GET /get_ecowitt_account` | login | Account, registration, device list metadata |
| `POST /set_ecowitt_account` | login | `{ account, password }` |
| `POST /ecowitt_register` | login | `{ name }` |
| `POST /ecowitt_logout` | login | Clears the stored account |
| `GET /get_sky_stats` | login | Cached sky JSON, or a fresh one with `?fresh=1` |
| `GET /get_sky_cfg` | login | Sky settings |
| `POST /set_sky_cfg` | login | Sky settings |

`/stream` uses the boundary `123456789000000000000987654321` and sends `Access-Control-Allow-Origin: *`, as `/capture` does. The stream handler returns when a frame cannot be taken or the client goes away. It is one client at a time in practice, because the single frame buffer is held for the duration of each JPEG send.

## Building

The project targets ESP-IDF 5.x on ESP32 (not S2/S3/C3). `sdkconfig.defaults` selects the chip, the 4 MB flash, the partition table, PSRAM, 240 MHz, the 15 dBm PHY cap, the legacy SCCB driver on I2C port 1 at 200 kHz, HTTP server tuning, and HTTP OTA. The legacy SCCB driver is required: on this board the OV2640 does not ACK the newer `i2c.master` driver.

Components pulled by the IDF component manager, from `firmware/main/idf_component.yml`:

- `espressif/esp32-camera` `^2.0.4`
- `espressif/esp_websocket_client` `^1.4.0`
- `espressif/mdns` `^1.8.0`

`espressif/esp_jpeg` is required by the main component as well.

From an ESP-IDF exported shell, in `firmware/`:

```bash
idf.py set-target esp32
idf.py build
```

`set-target` generates `sdkconfig` from `sdkconfig.defaults` the first time. The version string is the contents of `firmware/version.txt`. Change that file before building a release; once WittSky is running, its own OTA check is a straight string compare against that version.

To regenerate the time-zone table after pulling a newer POSIX zone database, run `firmware/main/gen_tz_zones.py`. It rewrites `tz_zones.c`.

The image can be installed in either of two ways. A serial flash writes the bootloader, the partition table, and the app. Spoofing the stock OTA check installs the app alone, into the inactive OTA slot, without opening the case. After WittSky is on the camera, later updates use the System page or serial again. The spoof server speaks the stock version check, which is a different JSON document from the one WittSky’s own updater expects.

### Serial

```bash
idf.py -p PORT flash monitor
```

`idf.py flash` uses the usual esptool offsets. `PORT` is the board’s serial device.

### Spoofing the stock OTA check

A camera still running the stock HP10 firmware asks `ota.ecowitt.net` for `GET /api/ota/v1/version/info`, then downloads whatever URL comes back in `data.attach1file`. `OTA_SPOOF/` answers that check from a PC. It brings up a 2.4 GHz WPA2 access point, hands the camera an address by DHCP, and resolves only `ota.ecowitt.net` and `oss.ecowitt.net` to that PC. Every other name is NXDOMAIN. The version body uses the stock field names (`data.name`, `data.content`, `data.attach1file`, `data.queryintval`). `attach1file` is an `https://oss.ecowitt.net/` URL; the stock client rewrites it to HTTP on port 80 and the same process serves the file.

`FW_VERSION` has to be a string the stock UI will treat as newer than `V1.1.1`. The default in the example settings is `V9.9.9`. The file it serves must be the application image, `firmware/build/hp10_bringup.bin`, not an ELF and not a full-chip dump.

On Windows, from an elevated PowerShell:

1. Copy `OTA_SPOOF/settings.env.example` to `OTA_SPOOF/settings.env`. Set `SSID`, `PSK` (8–63 characters), and `FW_PATH`. A relative `FW_PATH` is resolved from the `OTA_SPOOF` folder. The example points at `firmware/fw.bin` inside that folder, so copy `firmware/build/hp10_bringup.bin` there, or set `FW_PATH` to `../firmware/build/hp10_bringup.bin`. An absolute path works too.
2. In one elevated window, run `OTA_SPOOF/run.ps1`. It starts a hosted-network access point on a hosted-network-capable adapter, assigns `AP_IP` (default `192.168.50.1/24`), and opens the firewall for DHCP, DNS, and HTTP. It holds that network until Ctrl+C.
3. In a second elevated window, from the repository root, run `python -m OTA_SPOOF.main`. That process is the DHCP server, the DNS server, and the HTTP server on port 80. The raw traffic log also needs the elevated token. Ctrl+C stops it. Stopping `run.ps1` tears the access point down.

Point the camera’s station at that SSID. The stock Network page, reached through the camera’s own `HP10-WIFI` access point, is the usual way to do it. When the camera has a lease, open its web UI on the address DHCP just gave it and run the stock firmware upgrade. The spoof does not press that button for you. `AUTO_TRIGGER` is read from the settings file and is not acted on.

`queryintval` is returned in seconds. The stock firmware keeps its own default unless the value is between 300 and 86368 inclusive.

## First connection

1. Power the camera. The status LED is active low and lights once the sensor has initialised. If it stays off, the camera failed and the web UI is still available.
2. Join the open network `HP10-WIFIxxxx`, where `xxxx` is the last two bytes of the station MAC.
3. Open `http://192.168.4.1/` and log in. With no password set, any password works, including an empty one.
4. On Network, scan and join the 2.4 GHz router. When the page shows a station IP, `http://<hostname>.local` also works. The default hostname is `camera`.
5. On Capture, set latitude, longitude, and a time zone. Wait until the date field shows a real clock. Sunrise and sunset fill in from that.
6. If the pictures should appear on Ecowitt.net, log in with that account and register the camera, then enable **Ecowitt.net** and choose an interval. Otherwise enable **Custom URL**, set an `http` or `https` endpoint, and choose an interval.
7. On Sky, enable statistics if you want cloud cover and the light index. Point the mask at sky, not at the roof line. Leave white balance on Auto unless you have a reason to pin a preset.
8. On Camera, frame the shot, then save. Reboot if you changed resolution.
9. Set an access-point password on Network if the camera will stay reachable on the open `HP10-WIFI` network. That reboot also makes the password the login password.
10. On System, confirm the watchdog limits, and optionally set an OTA URL or turn on the log stream.

**Upload now** on the Status page is the quickest way to prove the path before waiting for the first scheduled shot.
