# Install WittSky without opening the camera

A stock HP10 checks Ecowitt's servers for a firmware update, then downloads whatever file those servers name. This folder makes your PC play that role on a small Wi-Fi network of its own. The camera joins that network, asks for an update, and is handed `WittSky_<version>.bin`. Nothing is soldered and the case stays closed.

Do this only on a camera you own, on a network you control. The access point is isolated on purpose: the camera can talk to your PC, and to nothing else.

## What the camera does, in plain terms

When you press the stock upgrade button, the camera makes two requests:

1. It asks `ota.ecowitt.net` whether a newer version exists.
2. If the answer says yes, it downloads the file from `oss.ecowitt.net`.

Both names have to land on your PC. If only the first one does, the camera will report a new version and then fail to download it. The camera also turns the download link from `https` into plain `http` on port 80, so your PC can serve the file without a certificate.

Your PC runs four small services to make that happen: a Wi-Fi access point, DHCP (so the camera gets an address and is told to use your PC for DNS), DNS (so those two Ecowitt names point at your PC), and a web server (the version answer and the `.bin` file).

## What you need

- Windows 11.
- A USB Wi-Fi adapter that can host a network. Most built-in laptop radios cannot. Check before you go further (next section).
- Python 3 on `PATH`.
- The WittSky application image, for example `WittSky_1.0.3.bin` from a local build (`firmware/build/`) or from the GitHub release. It must be the app `.bin`, not an ELF and not a full-chip dump.
- The camera still running stock firmware, and you able to open its web page (its own `HP10-WIFI` network) so you can point it at the lab network and start the upgrade.

## 1. Check that the adapter can host a network

Windows has two ways to share Wi-Fi. Mobile Hotspot will not work here: it runs its own DHCP and you cannot redirect `ota.ecowitt.net`. The method that works is the older "hosted network", and the adapter has to support it.

In an Administrator PowerShell window:

```powershell
netsh wlan show drivers
```

Look for this line:

```text
Hosted network supported  : Yes
```

If it says `No`, that adapter cannot be used. Recent Intel cards (an AX211, for example) report `No`.

The dongle used for this guide is an **Alfa AWUS036NEH**. On this PC, `netsh wlan show drivers` reports it as:

```text
Driver                    : 802.11n USB Wireless LAN Card
Vendor                    : Ralink Technology, Corp.
Provider                  : Microsoft
Date                      : 14/8/2007
Version                   : 5.1.22.0
INF file                  : netr28ux.inf
Hosted network supported  : Yes
```

If the PC has both a built-in radio and the dongle, Windows may try to host on the built-in one and fail with:

```text
The group or resource is not in the correct state to perform the requested operation.
```

Turn the built-in adapter off so the dongle is the only choice. The name `WiFi` below is typical; check yours with `netsh interface show interface`.

```powershell
netsh interface set interface name="WiFi" admin=disable
```

Turn it back on when you are finished:

```powershell
netsh interface set interface name="WiFi" admin=enable
```

## 2. Put the firmware where the server expects it

From the `OTA_SPOOF` folder, copy the example settings and place the image next to them:

```powershell
cd OTA_SPOOF
copy settings.env.example settings.env
copy ..\firmware\build\WittSky_1.0.3.bin firmware\fw.bin
```

`firmware\` under `OTA_SPOOF` is the default location. It is gitignored. If you would rather point at the build output directly, set `FW_PATH` in `settings.env` to that file. A relative path is resolved from the `OTA_SPOOF` folder.

Open `settings.env` and set at least these:

| Setting | What to put |
| --- | --- |
| `SSID` | Name of the lab network. No spaces. The camera will join this. |
| `PSK` | Password, 8 to 63 characters. The network is WPA2. |
| `FW_VERSION` | A version the stock UI will treat as newer than what is installed. `V9.9.9` is a safe choice against stock `V1.1.1`. |
| `FW_CONTENT` | The short note shown in the stock upgrade dialog. |
| `FW_PATH` | The image to serve. Default `firmware/fw.bin` is fine if you copied the file there. |

Leave `OTA_HOST`, `FW_HOST`, `AP_IP`, and the DHCP range alone unless you know you need different addresses. `AP_IP` (`192.168.50.1` by default) is the address of your PC on this network. The camera is told to use that same address as its gateway and its DNS server.

## 3. Start the access point

Open **PowerShell as Administrator**, go to `OTA_SPOOF`, and leave this window running:

```powershell
cd OTA_SPOOF
.\run.ps1
```

If Windows blocks the script:

```powershell
powershell -ExecutionPolicy Bypass -File .\run.ps1
```

The script creates the hosted network, gives it `AP_IP`, and opens the firewall for DHCP, DNS, and HTTP. It stays in the foreground until you press Ctrl+C, and it tears the network down when it exits.

You should see a line like `AP is up. Point the HP10 at SSID 'hp10lab'.`

## 4. Start the update server

Open a **second Administrator PowerShell**, from the repository root:

```powershell
python -m OTA_SPOOF.main
```

From inside `OTA_SPOOF` this also works:

```powershell
python .\main.py
```

It has to be elevated. DHCP and DNS bind to privileged ports, and the traffic log opens a raw socket. Leave this window running too.

On startup it prints the address it is spoofing, the version it will offer, and the size of the firmware file. If it says `FAIL firmware missing`, the path in `FW_PATH` is wrong. Fix that before you continue.

## 5. Point the camera at the lab network and upgrade

1. Join the camera's own `HP10-WIFI` network and open its web page.
2. On the stock Network page, join the lab SSID (`hp10lab`, or whatever you set).
3. In the Python window you should see a DHCP line within a few seconds, offering an address such as `192.168.50.50`.
4. Open the camera again, this time on the address it just received, and run the stock firmware upgrade (**Check firmware**, then **Upgrade**).

The script does not press that button for you.

A successful run looks like this in the Python window:

```text
[dhcp] ... REQUEST ... -> ACK 192.168.50.50 (dns 192.168.50.1)
[dns]  192.168.50.50 A ota.ecowitt.net -> 192.168.50.1
[http] 192.168.50.50 version/info -> V9.9.9 ...
[dns]  192.168.50.50 A oss.ecowitt.net -> 192.168.50.1
[http] 192.168.50.50 HEAD firmware (1714144 bytes)
[http] 192.168.50.50 GET  firmware (1714144 bytes)
```

The first DNS line is the version check. The second DNS line, for `oss.ecowitt.net`, means the camera accepted the offer and came back for the file. `HEAD` then `GET` is the download. After that the camera checks the image and reboots into it.

## If something does not happen

**The hosted network will not start.** The radio Windows picked cannot host. Disable the built-in adapter (step 1) so only the dongle is left, then run `run.ps1` again.

**The camera joins, but there is no DHCP line.** The Python window is not running, or it is not elevated. Both windows have to be Administrator.

**Check firmware says you are already on the latest version.** The camera caches the last answer. Power-cycle it and check again. Also confirm `FW_VERSION` is a string the stock UI will treat as newer than the installed one (`V9.9.9` against `V1.1.1`).

**A new version is offered, then the download fails, and there is no `HEAD` line.** The camera is fetching the file from the real `oss.ecowitt.net` instead of your PC. Leave `FW_HOST=oss.ecowitt.net` as it is in the example, and do not change it to a name you do not also spoof.

**The download finishes and the camera rejects the file.** The bytes were not an application image. Serve `WittSky_<version>.bin` (or a dump of a single OTA slot). An ELF or a full 4 MB chip dump will be refused.

## When you are done

Press Ctrl+C in both windows. `run.ps1` removes the firewall rules and stops the hosted network as it exits. Then turn the built-in adapter back on:

```powershell
netsh interface set interface name="WiFi" admin=enable
```

The camera should now be running WittSky. Later updates use the System page on the camera, or this same procedure again. If the camera will not boot at all, the serial recovery in the repository [README](../README.md#recovery--restoring-stock-firmware) puts stock firmware back.
