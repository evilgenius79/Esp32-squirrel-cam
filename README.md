# esp32-squirrel-cam

Squirrel-nest monitoring rig: an ESP32-class board with a camera and an
MLX90640 thermal array, streaming a live view over WiFi. The board hosts
its own webpage — **any phone or laptop on the LAN just opens
`http://squirrelcam.local/` to see the feed.** No companion app required.

```
┌──────────── in the nest ─────────────┐        ┌──── anywhere on LAN ─────┐
│  ESP32 + camera + MLX90640 (I²C)     │  WiFi  │  Phone / laptop / Pi in  │
│  serves /stream + /thermal + webpage │ ─────▶ │  kiosk mode: a browser   │
└──────────────────────────────────────┘        └──────────────────────────┘
```

## Two firmware options

Pick based on the hardware you have; they're independent.

| Path | Board | Camera | Notes |
|------|-------|--------|-------|
| **`firmware_p4/`** *(recommended)* | Waveshare **ESP32-P4-Nano** | Raspberry Pi Cam v1.3 (OV5647, MIPI-CSI) | Hardware JPEG, 720p+ feasible, multiple viewers OK. ESP-IDF 5.4+. |
| **`firmware/`** | Seeed **XIAO ESP32S3 Sense** | Onboard OV2640 / OV3660 | Tiny, simpler build, Arduino/PlatformIO. Limited to ~1 viewer at VGA. |

Each directory has its own README and build steps. Both expose the same
HTTP API:

| Endpoint | Content |
|----------|---------|
| `GET /`         | The webpage (video + live thermal heatmap). |
| `GET /stream`   | `multipart/x-mixed-replace` MJPEG. |
| `GET /snapshot` | Single JPEG. |
| `GET /thermal`  | JSON: `{w:32, h:24, seq, min, max, data:[...]}` in Celsius. |

## Hardware

Both builds share the same thermal sensor wiring concept, just on different
pins:

| Board | SDA | SCL | Docs |
|-------|-----|-----|------|
| ESP32-P4-Nano | GPIO7 | GPIO8 | [docs/wiring_p4.md](docs/wiring_p4.md) |
| XIAO ESP32S3 Sense | GPIO5 (D4) | GPIO6 (D5) | [docs/wiring.md](docs/wiring.md) |

MLX90640: 3V3, GND, SDA, SCL. I²C address `0x33` (factory, not
configurable). Most breakouts have 4.7 kΩ pull-ups already.

## Using it

Flash the firmware (see the chosen subdirectory's README), then on any
device on the same WiFi:

```
http://squirrelcam.local/
```

If mDNS doesn't resolve (some Android versions, restrictive routers), use
the IP printed in the serial log instead.

## Repo layout

```
firmware_p4/       ESP32-P4-Nano build (ESP-IDF). Primary.
  main/            camera, thermal, wifi, http, embedded webpage
  README.md

firmware/          XIAO ESP32S3 Sense build (PlatformIO/Arduino). Fallback.
  src/, include/
  platformio.ini

docs/
  wiring_p4.md     P4 pin map and mounting notes
  wiring.md        XIAO pin map and mounting notes

README.md          this file
```

## Security notes

Both firmwares target a **trusted home LAN**. No authentication, no TLS.
Anyone on the same WiFi can view the stream. Don't expose this to the
public internet without fronting it with a real reverse-proxy + auth. WiFi
credentials live in each firmware's config header — keep them out of
public git history (use `-D` build flags or a gitignored local override).

Both firmwares mitigate obvious LAN-side DoS:

- `/thermal` uses a single mutex-guarded static JSON buffer (no
  per-request malloc → no heap fragmentation under a request flood).
- JSON builder checks every `snprintf` return and bails to a 500 on
  overflow instead of leaning on a tight margin.
- MJPEG header builder uses a signed `int` and validates the return so a
  hypothetical encoding error can't underflow a size.

No SQL, no shell-outs, no file serving. The only file I/O is the embedded
HTML in flash (read-only). The camera's SCCB (sensor I²C) is on a separate
bus from the user I²C, so the thermal sensor never contends with the
camera.
