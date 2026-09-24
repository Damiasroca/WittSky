# HP10 firmware replacement via OTA server impersonation

This document describes how to install arbitrary firmware on an HP10 weather
camera by impersonating Ecowitt's over-the-air update infrastructure on a
laptop-controlled network. No JTAG, serial bootloader, or physical disassembly
is required. The camera's stock OTA client resolves its update endpoints through
whatever DNS the network hands it; by owning that network you redirect those
endpoints to the host, return a crafted version manifest, and serve a firmware
image of your choosing.

Scope: this targets hardware you own on a network you control. Everything below
assumes an isolated bench setup.

## Principle of operation

The stock client performs two independent HTTP exchanges against two distinct
Ecowitt hosts:

- a **version check** against `ota.ecowitt.net` (the manifest API), and
- an **artifact download** against `oss.ecowitt.net` (the object store holding
  the binary).

Both must be redirected. Spoofing only the manifest host gets you a "new version
available" state that then fails at download, because the image URL in the
manifest resolves to a host you don't control. The client also downgrades the
artifact URL from `https` to `http` before fetching, so the entire transaction
happens in cleartext on port 80 — which is precisely why a host with no TLS
termination and no trusted certificate can complete it.

The interception is assembled from four cooperating services on the host: DHCP
(to place the camera on the subnet and advertise the host as its resolver), DNS
(to answer the two Ecowitt hostnames), and an HTTP server that serves both the
manifest and the image. A raw-socket traffic tap runs alongside for observability.

## Requirements

- Windows 11 host.
- A USB WLAN adapter that advertises Windows hosted-network support (see below).
  The built-in adapter is, on current hardware, unlikely to qualify.
- The HP10 (validated here against stock V1.0.9) and access to its Wi-Fi
  provisioning mode.
- Python 3 available on `PATH`.
- The project tree: `run.ps1`, `settings.env`, and the core modules
  (`__main__.py`, `dhcp.py`, `dns.py`, `http_ota.py`, `settings.py`, `tap.py`).
- The replacement image at `firmware/fw.bin`.

## Adapter constraints

Windows exposes two mechanisms for acting as an access point, and only one of
them leaves DHCP and DNS under your control:

- **Mobile Hotspot** runs on top of Internet Connection Sharing. ICS imposes its
  own DHCP scope and pins clients to a fixed resolver at `192.168.137.1` with no
  per-host override. It cannot be used to redirect a specific hostname.
- **Hosted network** (the legacy SoftAP exposed through `netsh wlan
  set/start hostednetwork`) creates a virtual adapter you address directly. Bring
  it up *without* ICS and Windows runs no DHCP or DNS on it, which is exactly the
  blank slate this setup depends on.

Hosted network is therefore the required path, and it depends on driver support
that recent Intel parts no longer expose. Confirm before committing to a device:

```
netsh wlan show drivers
```

The determining line is:

```
Hosted network supported  : Yes
```

An Intel AX211 reports `No`. A Ralink- or Realtek-based dongle that still reports
`Yes` is the workable option. Treat that line as a hard gate — without it, none
of the following applies.

## Radio arbitration

Hosted network is a single, global instance, and Windows binds it to one radio
of its choosing. With two capable-looking adapters present it may bind to the
built-in card, and if that card cannot host, `netsh wlan start hostednetwork`
fails with:

```
The group or resource is not in the correct state to perform the requested operation.
```

This is a binding-target problem, not a driver fault. Remove the ambiguity by
disabling the built-in interface so the dongle is the only candidate. Do this
**before** invoking `run.ps1`:

```
netsh interface set interface name="WiFi" admin=disable
```

`WiFi` is the built-in AX211 here; the dongle enumerated as `WiFi 2`. Confirm the
names on your host with `netsh interface show interface`. Restore the built-in
adapter when finished:

```
netsh interface set interface name="WiFi" admin=enable
```

## Configuration

Edit `settings.env`. The values that carry meaning for the interception:

- `SSID` / `PSK` — the AP identity. Hosted network is WPA2-PSK/CCMP only; the
  passphrase must be 8–63 characters.
- `AP_IP` — the host's address on the AP subnet. It serves simultaneously as
  gateway, resolver, and HTTP origin; every redirected hostname points here.
- `OTA_HOST` (`ota.ecowitt.net`) — the manifest host.
- `FW_HOST` (`oss.ecowitt.net`) — the artifact host. Both `OTA_HOST` and
  `FW_HOST` resolve to `AP_IP`.
- `FW_VERSION` — returned as `data.name` and compared against the running
  version. It must outrank what's installed for the client to treat it as new;
  `V9.9.9` is a safe sentinel.
- `FW_PATH` — the served image, relative to the project root or absolute.
- `QUERY_INTVAL` — returned as `data.queryintval`. Values outside 300–86368 are
  ignored by the client, which then falls back to its default poll interval.

## Bring-up

1. Disable the built-in WLAN adapter (previous section) and connect the dongle.

2. **Terminal 1 (elevated) — access point:**

   ```
   .\run.ps1
   ```

   This starts the hosted network (WPA2/CCMP, no ICS), waits for the virtual
   adapter, assigns `AP_IP` to it, enables the weak host model on the relevant
   interfaces, and opens inbound firewall rules for UDP 53/67 and TCP 80. If
   script execution is blocked, invoke it as
   `powershell -ExecutionPolicy Bypass -File .\run.ps1` rather than relaxing the
   machine policy. Leave it resident; teardown happens on exit.

3. **Terminal 2 (elevated) — service core:**

   ```
   python .\__main__.py
   ```

   Elevation is mandatory: the tap opens a raw socket in promiscuous mode
   (`SIO_RCVALL`), and DHCP/DNS bind privileged ports. The core starts the tap,
   DHCP, DNS, and HTTP services.

4. Join the camera to the configured SSID through its provisioning flow. A lease
   should appear in the DHCP log within a few seconds.

5. Trigger the update, either from the camera's web UI (**Check firmware**, then
   **Upgrade**) or directly:

   ```
   Invoke-WebRequest -Uri http://<camera-ip>/upgrade_process -Method POST -Body '{"upgrade":"check"}' -ContentType application/json
   Invoke-WebRequest -Uri http://<camera-ip>/upgrade_process -Method POST -Body '{"upgrade":"start"}' -ContentType application/json
   ```

## Expected transaction

A successful run produces this sequence in the core log:

```
[dhcp] ... REQUEST ... -> ACK 192.168.50.50 (dns 192.168.50.1)
[dns]  192.168.50.50 A ota.ecowitt.net -> 192.168.50.1
[http] 192.168.50.50 version/info -> V9.9.9 ...
[dns]  192.168.50.50 A oss.ecowitt.net -> 192.168.50.1
[http] 192.168.50.50 HEAD firmware (1520448 bytes)
[http] 192.168.50.50 GET  firmware (1520448 bytes)
```

Each line is diagnostic. The ACK confirms the camera accepted the host as its
resolver. The `ota.ecowitt.net` lookup and `version/info` response confirm the
manifest exchange. The `oss.ecowitt.net` lookup is the pivotal one: it means the
client parsed the manifest, accepted the offered version, and returned to fetch
the artifact. The `HEAD` and `GET` complete the transfer, after which the client
validates and commits the image.

## Protocol detail

**DHCP.** Offers and acknowledgements carry options 53 (message type), 54
(server identifier), 51 (lease time), 1 (subnet mask), 3 (router), and 6 (DNS).
Router and DNS are both set to `AP_IP`; option 6 is what routes the camera's
name resolution through the host. Replies are broadcast to `255.255.255.255:68`.

**DNS.** The resolver answers `A` queries for `OTA_HOST` and `FW_HOST` with
`AP_IP` and returns `NXDOMAIN` for everything else. Routine noise (NTP hostnames)
is resolved-as-refused without logging to keep the trace readable.

**Version manifest.** `GET /api/ota/v1/version/info` returns JSON with `code: 0`
and a `data` object containing `name` (the offered version, compared against the
running one), `content` (changelog text surfaced in the UI), `attach1file` (the
image URL), and `queryintval`. The `is_new` decision hinges on the version
comparison against `data.name`; a manifest that reaches the client but offers an
equal-or-lower `name` yields "latest version" rather than an upgrade prompt.

**Artifact fetch.** `attach1file` is expressed as an `https://` URL on `FW_HOST`.
The client rewrites the scheme to `http` and fetches on port 80. It first issues
a `HEAD` to size the object; that response must present the literal status line
`HTTP/1.1 200 OK` and a `Content-Length:` header, or the client reads the size as
zero and aborts with "Get Firmware failed." A correct `Content-Length` on both
the `HEAD` and the subsequent `GET` is therefore mandatory. The server streams
the image on the `GET`, and the client flashes on completion.

## Operational notes and failure modes

- **`start hostednetwork` reports "not in the correct state."** The instance
  bound to a non-hosting radio. Disable the built-in adapter so the dongle is the
  sole candidate.
- **The web check reports the running version as current despite a changed
  manifest.** The client caches the check result and only re-queries on reboot or
  its own timer. Power-cycle the camera to force a fresh fetch after any manifest
  change.
- **`is_new` remains false though the check reached the host.** V1.0.9 reads the
  offered version from `data.name`; ensure it is present and outranks the
  installed version. Older reconstructions referenced `data.version`; populating
  both is harmless if the field in use is uncertain.
- **"Get Firmware failed" with no HEAD in the log.** `FW_HOST` is unspoofed, so
  the artifact hostname resolves off-host and the `HEAD` never reaches you.
  Redirect `oss.ecowitt.net` to `AP_IP` and point `attach1file` at it.
- **UDP services drop after a burst of refused lookups.** An ICMP
  port-unreachable elicited by a prior datagram causes Windows to surface
  `WSAECONNRESET` on the next `recvfrom`. The core suppresses this via
  `SIO_UDP_CONNRESET`; any reimplementation must do the same or the sockets will
  fault under load.
- **Traffic between the camera and `AP_IP` behaves inconsistently across the two
  adapters.** `run.ps1` enables the weak host model (`weakhostsend` /
  `weakhostreceive`) so the host accepts and originates packets on `AP_IP`
  regardless of which interface handles them. This is required in the dual-adapter
  topology.
- **The tap prints nothing.** Raw-socket capture requires an elevated context;
  run Terminal 2 as Administrator.
- **The transfer completes but the flash is rejected.** This is past the network
  boundary. The bootloader validates the image descriptor before committing, and
  secure boot — if provisioned — rejects unsigned images irrespective of how they
  were delivered. Build with an application version that exceeds the running one
  to avoid a rollback rejection, and confirm secure boot is disabled before
  expecting an unsigned image to be accepted.

## Teardown

Interrupt both terminals with Ctrl+C. `run.ps1` removes the firewall rules,
releases `AP_IP`, and stops the hosted network on exit. Restore the built-in
adapter:

```
netsh interface set interface name="WiFi" admin=enable
```
