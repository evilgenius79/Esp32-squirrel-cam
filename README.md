# esp32-squirrel-cam

Squirrel-nest camera built around the **Seeed Studio XIAO ESP32S3 Sense**
(OV2640/OV3660 camera + PSRAM) and a **MLX90640** 32×24 thermal array.
The ESP32 serves an MJPEG video stream and a JSON thermal feed; a
**Raspberry Pi 5** with the official 7" DSI touchscreen displays them side
by side.

```
  ┌───────────────────────┐        WiFi         ┌──────────────────────┐
  │ XIAO ESP32S3 Sense    │  ─ /stream  (MJPEG) │ Raspberry Pi 5       │
  │  + OV2640/OV3660 cam  │  ─ /thermal (JSON)  │  + 7" DSI display    │
  │  + MLX90640 (I2C)     │ ─────────────────▶  │  + squirrel_viewer   │
  └───────────────────────┘                     └──────────────────────┘
```

## Repo layout

```
firmware/        PlatformIO project for the XIAO ESP32S3 Sense
pi_viewer/       Python viewer + systemd unit for the Pi 5
docs/wiring.md   Wiring diagram and pin map
```

## Hardware

See [docs/wiring.md](docs/wiring.md) for the full pin map. Short version:

| XIAO pad | GPIO | MLX90640 |
|----------|------|----------|
| 3V3      | —    | VIN      |
| GND      | —    | GND      |
| D4 / SDA | 5    | SDA      |
| D5 / SCL | 6    | SCL      |

The camera's SCCB bus (GPIO39/40) is internal and independent from the user
I2C bus, so the thermal sensor doesn't clash with it.

## Firmware (XIAO ESP32S3 Sense)

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or CLI).
2. Edit `firmware/include/config.h` and set `WIFI_SSID` / `WIFI_PASSWORD`.
   (Or pass them as build flags, e.g.
   `PLATFORMIO_BUILD_FLAGS='-DWIFI_SSID=\"foo\" -DWIFI_PASSWORD=\"bar\"'`.)
3. Plug the XIAO in over USB-C and build + upload:

   ```sh
   cd firmware
   pio run -t upload
   pio device monitor
   ```

   On first boot the serial log prints the assigned IP and mDNS name, e.g.
   `ready: open http://192.168.1.42/ or http://squirrelcam.local/`.

### HTTP endpoints

| Path        | Content                                                 |
|-------------|---------------------------------------------------------|
| `GET /`         | Minimal HTML landing page with the live `<img>` feed.   |
| `GET /stream`   | `multipart/x-mixed-replace` MJPEG video.                |
| `GET /snapshot` | Single JPEG frame.                                      |
| `GET /thermal`  | JSON `{w,h,min,max,seq,data:[...]}` (Celsius, 32×24).   |

### Knobs in `config.h`

- `CAMERA_FRAMESIZE` — `FRAMESIZE_VGA` by default. Bigger frames eat WiFi
  bandwidth fast.
- `JPEG_QUALITY` — 10–15 is a good range; lower is higher quality.
- `MLX90640_REFRESH_HZ` — 0.5, 1, 2, 4, 8, 16, 32, or 64. Eight hertz is a
  comfortable default; push it higher only if your I2C bus is short and clean.

## Viewer (Raspberry Pi 5)

```sh
cd pi_viewer
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python3 squirrel_viewer.py --host squirrelcam.local
```

Hotkeys: `q` or `Esc` to quit, `f` to toggle fullscreen.

Use `--windowed` while you're iterating, and `--width/--height` to match a
non-default display (e.g. `--width 1024 --height 600`).

### Run on boot

Copy the unit file and enable it:

```sh
sudo cp pi_viewer/squirrel-cam.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now squirrel-cam.service
```

The unit assumes a Raspberry Pi OS desktop session owned by user `pi`; adjust
`User=` and the `DISPLAY`/`XAUTHORITY` variables if your setup differs.

## Security notes

This firmware is designed for a **trusted home LAN**. In particular:

- **No authentication and no TLS.** Anyone on the same WiFi can open
  `/stream`, `/snapshot`, and `/thermal`. Keep the device on your private
  network and don't port-forward it. If you need remote access, tunnel
  through a VPN or reverse-proxy it with auth on the Pi.
- **WiFi credentials live in `firmware/include/config.h`.** Don't commit your
  real credentials. Either keep them out of git (e.g. add a
  `config.local.h` that you `#include` and `.gitignore`) or pass them as
  build flags:

  ```sh
  PLATFORMIO_BUILD_FLAGS='-DWIFI_SSID=\"mynet\" -DWIFI_PASSWORD=\"secret\"' \
    pio run -t upload
  ```

- `CORS: *` is set on every endpoint so the Pi viewer (and your laptop
  browser) can fetch JSON from a different origin. Same-network trust
  still applies.
- The Pi viewer caps both the MJPEG reassembly buffer (4 MiB) and the
  `/thermal` response (32 KiB) so a malfunctioning or malicious server on
  the same LAN can't OOM it.
- The systemd unit runs as the `pi` user (not root). Leave it that way.

## Troubleshooting

- **"MLX90640 not found"** — check 3V3, SDA/SCL wiring, and that you wired to
  D4/D5 (GPIO5/6), not to the camera's internal SCCB pins. Run `i2cdetect` on
  a USB-to-I2C bridge if in doubt; the sensor should answer at `0x33`.
- **Camera init fails / brownout** — USB port can't supply enough current. Use
  a proper 5 V / 1 A supply, not a laptop hub.
- **Stream stalls after a minute** — the XIAO's WiFi goes to sleep on some
  routers. The firmware disables WiFi sleep, but a weak signal near a metal
  nest box will still drop frames. Move the ESP32 closer or add an external
  antenna (the XIAO has a u.FL pad, though you have to move the 0 Ω jumper).
- **Thermal frame looks mirrored** — expected; the viewer flips the MLX90640
  horizontally so it matches the camera's orientation. If your mounting has
  the sensor rotated, edit `render_thermal()` in `squirrel_viewer.py`.
