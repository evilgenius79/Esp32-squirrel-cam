# Wiring

## Bill of materials

| Part | Notes |
|---|---|
| Seeed Studio XIAO ESP32S3 **Sense** | Includes the OV2640/OV3660 camera daughterboard and PSRAM. Don't buy the non-Sense variant — it has no camera. |
| MLX90640 breakout (32×24 thermal array) | Adafruit #4469 (55° FOV) or similar. 3.3 V, I2C addr `0x33`. |
| 4× jumpers (female-to-female) | Or a 4-wire JST-SH pigtail if you want to pot the nest end. |
| USB-C cable + 5 V supply | The XIAO draws ~250 mA peak while streaming. |
| Raspberry Pi 5 + official 7" DSI display | Any resolution works; viewer defaults to 800×480. |

## XIAO ESP32S3 Sense ↔ MLX90640

The camera has its own private SCCB bus on internal pins GPIO39/40, so it does
**not** conflict with the user I2C bus. Wire the thermal sensor to the pads
labelled `SDA` and `SCL` on the XIAO silkscreen:

```
XIAO pad   | XIAO GPIO | MLX90640 pin
-----------+-----------+--------------
3V3        | —         | VIN / VCC
GND        | —         | GND
D4 / SDA   | GPIO5     | SDA
D5 / SCL   | GPIO6     | SCL
```

Keep the I2C leads under ~15 cm; the driver runs the bus at 1 MHz (FM+) to
keep up with 8 Hz thermal refresh. If your breakout lacks onboard pull-ups,
add 4.7 kΩ from SDA and SCL to 3V3 each.

## Pin map (for reference)

Camera (internal, owned by the esp32-camera driver):

```
XCLK =GPIO10  PCLK =GPIO13  VSYNC=GPIO38  HREF =GPIO47
SIOD =GPIO40  SIOC =GPIO39  (SCCB — do not reuse)
Y2..Y9 = GPIO15,17,18,16,14,12,11,48
```

User I/O exposed on the castellated pads:

```
D0 GPIO1   D1 GPIO2   D2 GPIO3   D3 GPIO4
D4 GPIO5  (SDA)       D5 GPIO6  (SCL)
D6 GPIO43 (TX)        D7 GPIO44 (RX)
D8 GPIO7  (SCK)       D9 GPIO8  (MISO)  D10 GPIO9 (MOSI)
```

## Mounting in the nest box

- Aim the camera straight at the expected entry or the floor centre.
- The MLX90640 has a 55° × 35° FOV on the common breakout; mount it as close
  to the camera's optical axis as practical so the thermal overlay lines up.
- Bring the USB-C cable out through a small notch and silicone around it.
  Keep the electronics on the upper, drier side of the box.
