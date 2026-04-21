#pragma once

// -----------------------------------------------------------------------------
// User configuration
// -----------------------------------------------------------------------------

// WiFi credentials. Fill these in before flashing.
#ifndef WIFI_SSID
#define WIFI_SSID     "your-ssid"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "your-password"
#endif

// Static hostname advertised via mDNS (http://squirrelcam.local/).
#define MDNS_HOSTNAME "squirrelcam"

// HTTP server port.
#define HTTP_PORT 80

// MLX90640 I2C address (fixed from factory).
#define MLX90640_I2C_ADDR 0x33

// MLX90640 refresh rate. The sensor maxes out near 32 Hz over 1 MHz I2C, but
// 8 Hz is a good balance for readability and bus headroom alongside the camera.
// Valid values: 0.5, 1, 2, 4, 8, 16, 32, 64.
#define MLX90640_REFRESH_HZ 8

// I2C bus speed for the external thermal sensor. MLX90640 supports FM+.
#define I2C_FREQ_HZ 1000000UL

// User-accessible I2C pins on the XIAO ESP32S3. These are exposed on pads
// labelled SDA (D4) and SCL (D5) on the board silkscreen. They are NOT the
// camera's internal SCCB bus (which sits on GPIO39/40 and is owned by the
// esp32-camera driver).
#define PIN_I2C_SDA 5
#define PIN_I2C_SCL 6

// JPEG quality (lower = higher quality, larger frames). 10-12 is a reasonable
// compromise for streaming over WiFi.
#define JPEG_QUALITY 12

// Frame size. UXGA=1600x1200, SVGA=800x600, VGA=640x480, HQVGA=240x176.
// VGA works well for a 7" Pi display and keeps the frame rate usable.
#define CAMERA_FRAMESIZE FRAMESIZE_VGA
