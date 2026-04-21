# firmware_p4 — ESP32-P4-Nano build

ESP-IDF project for the **Waveshare ESP32-P4-Nano** with a Raspberry Pi v1.3
camera (OV5647) and an MLX90640 thermal array. The board serves a single
HTML page at `http://squirrelcam.local/` that renders the MJPEG video next
to a live 32×24 thermal heatmap — no separate viewer required. Any phone or
laptop on the LAN just opens the page.

## Architecture

```
┌──────────────── ESP32-P4-Nano ────────────────┐
│  P4 (RISC-V, app CPU)                         │
│  ├── CSI   ←─ OV5647 camera (MIPI, 2-lane)    │
│  ├── ISP  ───▶ RGB565                         │
│  ├── HW JPEG encoder ───▶ JPEG                │
│  ├── I2C0 ←─ MLX90640 (32×24 @ 8 Hz)          │
│  └── SDIO ──▶ ESP32-C6 (WiFi slave)           │
│               via esp_hosted / esp_wifi_remote│
└───────────────────────────────────────────────┘
            │ WiFi
            ▼
        Your LAN  ──▶  any browser
```

## Requirements

- **ESP-IDF v5.4** or newer (v5.5 recommended). v5.3 will not build — the
  P4 camera driver and `esp_wifi_remote` are not mature there.
- `idf.py` on your PATH, with the Python env activated
  (`. $IDF_PATH/export.sh`).
- The ESP32-C6 on the board needs matching **`esp_hosted` slave firmware**.
  Waveshare's factory image is usually a version behind. See
  *"Flash the C6 slave"* below.

## Build and flash

```sh
cd firmware_p4
idf.py set-target esp32p4
idf.py menuconfig       # first time only — set WiFi creds under "Example Config"
                        # (or edit WIFI_SSID / WIFI_PASSWORD in main/app_config.h)
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The USB port on the Nano enumerates as a standard USB-CDC device; no
external USB-to-serial adapter is required to flash the P4 itself.

## Flash the C6 slave (one-time)

If WiFi association fails with errors like `slave transport not responding`
or `esp_hosted: version mismatch`, you need to reflash the C6. The C6 UART
isn't broken out for direct USB flashing on the Nano, so use the SDIO-based
OTA flasher:

```sh
# Clone esp_hosted and build the C6 slave firmware for the IDF version you
# pulled via idf_component.yml:
git clone https://github.com/espressif/esp-hosted.git
cd esp-hosted/slave
idf.py set-target esp32c6
idf.py build
# Then flash the .bin to the C6 over SDIO using the P4 as a relay — see
# the slave README for the exact incantation; it changes between versions.
```

Once the slave version matches the host component version, WiFi works like
any other IDF app.

## Web UI

The page at `/` has a small mode switcher in the header:

| Mode      | What it shows                                                  |
|-----------|----------------------------------------------------------------|
| **Side**    | Video on the left, thermal heatmap on the right (default).  |
| **Overlay** | Thermal canvas blended over the video at ~55% opacity, screen blend mode. |
| **Off**     | Video only. Polling of `/thermal` stops, so the device is not asked to serialise a frame for nothing. |

The choice is remembered in `localStorage` per browser, so a refresh — or
the kiosk-mode browser on your wall display — keeps the last mode.

## HTTP endpoints

| Path        | What                                                      |
|-------------|-----------------------------------------------------------|
| `GET /`         | The webpage (HTML + inline JS + canvas thermal render).|
| `GET /stream`   | `multipart/x-mixed-replace` MJPEG video.               |
| `GET /snapshot` | Single JPEG.                                           |
| `GET /thermal`  | `{w,h,seq,min,max,data:[...]}` — 32×24 floats °C.      |

The same hardening as the XIAO firmware applies: no per-request malloc
for `/thermal`, bounded JSON builder, capped snprintf.

## Files you'll touch

- `main/app_config.h` — WiFi creds, frame size (`CAM_WIDTH`/`HEIGHT`),
  JPEG quality, thermal refresh rate, MLX90640 pins.
- `main/web/index.html` — the UI. Embedded into flash via `EMBED_FILES`
  in `main/CMakeLists.txt`.
- `main/mlx90640_driver.c` — simplified sensor read. Good enough for the
  heatmap. Swap in the full Melexis `MLX90640_API.c` if you want
  lab-accurate Celsius.

## Known-fragile spots

Honest disclaimer: the P4 camera + `esp_wifi_remote` stacks are fairly new
and the exact struct field names drift between IDF point releases. If the
build fails on something like `esp_cam_ctlr_csi_config_t` member not found,
open `$IDF_PATH/examples/peripherals/camera/mipi_isp_dsi` for the current
IDF's reference initializer and align `main/camera.c` against it. The
overall flow (sensor probe → CSI controller → JPEG encoder → task) is
stable.

See [../docs/wiring_p4.md](../docs/wiring_p4.md) for the pin map.
