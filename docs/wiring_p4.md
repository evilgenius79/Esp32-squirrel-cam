# Wiring — ESP32-P4-Nano build

Primary build path. If you're using the smaller XIAO ESP32S3 Sense instead,
see [wiring.md](wiring.md).

## Bill of materials

| Part | Notes |
|---|---|
| **Waveshare ESP32-P4-Nano** | P4 + onboard ESP32-C6 (WiFi6), 32 MB PSRAM, MIPI-CSI + MIPI-DSI. |
| **Raspberry Pi Camera v1.3** (OV5647) | 5 MP, MIPI-CSI, connects via FFC to the CSI port on the Nano. The v2 (IMX219) also works with a driver swap in `esp_cam_sensor`. |
| **MLX90640** breakout | 32×24 thermal array, I2C addr `0x33`. Adafruit #4469 or equivalent. |
| USB-C 5V / ≥1 A supply | The P4 + C6 + camera peaks well above what a laptop port delivers cleanly. |

## MLX90640 → ESP32-P4-Nano

The user-accessible I2C is broken out on the 2×26 GPIO header. These pins
are independent from the camera's SCCB bus and from the DSI touch I2C, so
nothing contends with the thermal sensor.

```
ESP32-P4-Nano | MLX90640
--------------+---------
3V3           | VIN / VCC
GND           | GND
GPIO7  (SDA)  | SDA
GPIO8  (SCL)  | SCL
```

Keep the I2C wires short (< 20 cm). The firmware drives the bus at 400 kHz,
which is conservative and works over typical jumper lengths. The MLX90640
supports 1 MHz if you need higher thermal refresh and have a clean bus.

Most MLX90640 breakouts include 4.7 kΩ pull-ups; if yours doesn't, add them
from SDA → 3V3 and SCL → 3V3.

## Camera

The Pi Cam v1.3 plugs into the Nano's MIPI-CSI FFC socket with the copper
contacts facing the board. The firmware drives the 2-lane interface at
720p RGB565 by default; the P4's ISP handles Bayer → RGB and the hardware
JPEG encoder compresses in a single pass.

If your camera is upside down in the nest, set `CAM_FLIP`/`CAM_MIRROR` in
`main/app_config.h` (TODO: add when the exact API exposes it per-sensor —
for now, mount the camera the right way up).

## GPIOs reserved by the Nano (do not reuse)

```
14, 15, 16, 17 : SDIO D0..D3 to the onboard ESP32-C6 (WiFi)
18             : SDIO CLK
19             : SDIO CMD
54             : ESP32-C6 RESET
34, 35         : Camera SCCB (sensor I²C, private)
```

Anything not listed here and not on the camera/DSI/Ethernet headers is fair
game for additional sensors.

## Mounting in the nest box

Same guidance as the XIAO build: aim the camera at the expected floor or
entry; the MLX90640 has a 55° × 35° FOV, so keep it close to the camera's
optical axis so hot blobs line up with the video. USB cable out through
a small notch, silicone the hole, keep the electronics on the dry upper
wall of the box.
